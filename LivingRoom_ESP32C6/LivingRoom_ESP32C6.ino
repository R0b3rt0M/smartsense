#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>
#include <WiFiManager.h> 
#include <Preferences.h> 
#include <esp_task_wdt.h>   // Watchdog nativo ESP-IDF
#include <esp_system.h>     // Causa del reset

// --- Timeout Watchdog Hardware (secondi) ---
#define WDT_TIMEOUT_SEC 8

// --- Configurazione MQTT Dinamica ---
char mqtt_server[40] = "";
char mqtt_port[6]    = "1883";
char mqtt_user[40]   = "";
char mqtt_pass[40]   = "";

// Topic MQTT
const char* mqtt_topic_env    = "smartsense/livingroom/environment";
const char* mqtt_topic_status = "smartsense/livingroom/status";

// --- Pin Hardware XIAO ESP32-C6 ---
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (GPIO3 - Uscita digitale LD2410C per fallback)
constexpr uint8_t RADAR_TX_PIN    = 16; // D6 (GPIO16 - XIAO TX -> LD2410C RX)
constexpr uint8_t RADAR_RX_PIN    = 17; // D7 (GPIO17 - XIAO RX <- LD2410C TX)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Pulsante 'B' integrato sullo XIAO

// Oggetti globali
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Adafruit_SHT4x sht4x = Adafruit_SHT4x();
BH1750 lightMeter;
Preferences prefs;
HardwareSerial RadarSerial(1);

// Motivo del boot
const char* bootReasonStr = "UNKNOWN";

// --- LAYER 1: STRUTTURA DATI INDIPENDENTE (Device State) ---
struct DeviceData {
  bool presence_raw         = false;
  bool presence_filtered    = false;
  uint8_t target_state      = 0;
  uint16_t dist_cm          = 0;
  uint16_t moving_dist_cm   = 0;
  uint8_t moving_energy     = 0;
  uint16_t stat_dist_cm     = 0;
  uint8_t stat_energy       = 0;
  bool radar_uart_ok        = false;

  float temp_c              = 0.0;
  float hum_pct             = 0.0;
  bool sht40_ok             = false;
  float lux                 = -1.0;
  bool bh1750_ok            = false;

  int32_t rssi              = 0;
  uint32_t free_heap        = 0;
  uint32_t min_free_heap    = 0;
  uint32_t uptime_sec       = 0;
} device;

// Buffer parser UART radar
uint8_t rxBuf[64];
uint8_t rxIdx = 0;
unsigned long lastRadarFrameTime = 0;

// Timer operativi e Non-Bloccanti
bool shouldSaveConfig = false;
bool ultimaPresenzaTrasmessa = false;
unsigned long lastTelemetryTime      = 0;
unsigned long lastMqttReconnectTime  = 0;
unsigned long lastRadarDetectionTime = 0;
unsigned long buttonPressStartTime   = 0;
bool isButtonPressed                 = false;

constexpr unsigned long TELEMETRY_INTERVAL      = 10000;
constexpr unsigned long MQTT_RECONNECT_INTERVAL = 5000;
constexpr unsigned long PORTAL_TRIGGER_TIME     = 3000;
constexpr unsigned long RADAR_TIMEOUT_MS        = 3000;
constexpr unsigned long PRESENCE_OFF_DELAY_MS   = 2500;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// Decodifica motivo del reset hardware
const char* ottieniMotivoReset() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_EXT:       return "EXTERNAL_PIN";
    case ESP_RST_SW:        return "SOFTWARE_RESTART";
    case ESP_RST_PANIC:     return "EXCEPTION_PANIC";
    case ESP_RST_INT_WDT:   return "INTERNAL_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT_TRIGGERED";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP_WAKE";
    case ESP_RST_BROWNOUT:  return "BROWNOUT_VOLTAGE_DROP";
    case ESP_RST_SDIO:      return "SDIO_RESET";
    case ESP_RST_USB:       return "USB_JTAG_RESET";
    default:                return "OTHER_UNKNOWN";
  }
}

// Decodifica stato target radar
const char* decodificaStatoRadar(uint8_t state) {
  switch (state) {
    case 0:  return "none";
    case 1:  return "moving";
    case 2:  return "stationary";
    case 3:  return "both";
    default: return "unknown";
  }
}

// --- LAYER 2: SENSOR DRIVERS ---
void aggiornaRadarUART() {
  while (RadarSerial.available()) {
    uint8_t b = RadarSerial.read();

    if (rxIdx < sizeof(rxBuf)) {
      rxBuf[rxIdx++] = b;
    } else {
      rxIdx = 0;
    }

    if (rxIdx == 1 && rxBuf[0] != 0xF4) rxIdx = 0;
    else if (rxIdx == 2 && rxBuf[1] != 0xF3) rxIdx = 0;
    else if (rxIdx == 3 && rxBuf[2] != 0xF2) rxIdx = 0;
    else if (rxIdx == 4 && rxBuf[3] != 0xF1) rxIdx = 0;

    if (rxIdx >= 6) {
      uint16_t dataLen = rxBuf[4] | (rxBuf[5] << 8);
      uint16_t totalFrameLen = 4 + 2 + dataLen + 4;

      if (totalFrameLen > sizeof(rxBuf) || dataLen < 11) {
        rxIdx = 0;
        continue;
      }

      if (rxIdx == totalFrameLen) {
        if (rxBuf[totalFrameLen - 4] == 0xF8 &&
            rxBuf[totalFrameLen - 3] == 0xF7 &&
            rxBuf[totalFrameLen - 2] == 0xF6 &&
            rxBuf[totalFrameLen - 1] == 0xF5) {
          
          if ((rxBuf[6] == 0x02 || rxBuf[6] == 0x01) && rxBuf[7] == 0xAA) {
            device.target_state    = rxBuf[8] & 0x03;
            device.moving_dist_cm  = rxBuf[9]  | (rxBuf[10] << 8);
            device.moving_energy   = rxBuf[11];
            device.stat_dist_cm    = rxBuf[12] | (rxBuf[13] << 8);
            device.stat_energy     = rxBuf[14];
            device.dist_cm         = rxBuf[15] | (rxBuf[16] << 8);
            lastRadarFrameTime     = millis();
            device.radar_uart_ok   = true;
          }
        }
        rxIdx = 0;
      }
    }
  }

  if (device.radar_uart_ok && (millis() - lastRadarFrameTime > RADAR_TIMEOUT_MS)) {
    device.radar_uart_ok = false;
  }
}

void leggiSensoriI2C() {
  device.sht40_ok = false;
  sensors_event_t hum, temp;
  if (sht4x.getEvent(&hum, &temp)) {
    if (!isnan(temp.temperature) && temp.temperature > -40.0 && temp.temperature < 85.0 &&
        !isnan(hum.relative_humidity) && hum.relative_humidity >= 0.0 && hum.relative_humidity <= 100.0) {
      device.temp_c = temp.temperature;
      device.hum_pct = hum.relative_humidity;
      device.sht40_ok = true;
    }
  } else {
    sht4x.begin(&Wire);
  }

  float l = lightMeter.readLightLevel();
  if (l >= 0.0 && l <= 65535.0) {
    device.lux = l;
    device.bh1750_ok = true;
  } else {
    device.bh1750_ok = false;
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  }
}

// --- LAYER 3: CONFIGURAZIONE & PORTALE ---
void avviaCaptivePortal(bool forzaApertura) {
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter custom_mqtt_server("server", "IP Broker MQTT (es. 192.168.1.174)", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "Porta MQTT (default 1883)", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "Utente MQTT (opzionale)", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "Password MQTT (opzionale)", mqtt_pass, 40);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);

  String customCss = "<style>"
                     "div[style*='text-align:center;'] { display: none !important; }"
                     "button { background-color: #27AE60 !important; color: white !important; border-radius: 8px !important; }"
                     "</style>";
  wifiManager.setCustomHeadElement(customCss.c_str());
  wifiManager.setTitle("SmartSense Setup");
  wifiManager.setDarkMode(true);
  wifiManager.setConfigPortalTimeout(180);

  bool connesso = forzaApertura ? wifiManager.startConfigPortal("SmartSense-Setup") 
                                : wifiManager.autoConnect("SmartSense-Setup");

  if (!connesso) {
    delay(2000);
    ESP.restart();
  }

  if (shouldSaveConfig) {
    String new_server = custom_mqtt_server.getValue();
    String new_port   = custom_mqtt_port.getValue();
    new_server.trim();
    new_port.trim();
    int port_num = new_port.toInt();

    if (new_server.length() > 0 && port_num > 0 && port_num <= 65535) {
      strlcpy(mqtt_server, new_server.c_str(), sizeof(mqtt_server));
      strlcpy(mqtt_port,   new_port.c_str(),   sizeof(mqtt_port));
      strlcpy(mqtt_user,   custom_mqtt_user.getValue(), sizeof(mqtt_user));
      strlcpy(mqtt_pass,   custom_mqtt_pass.getValue(), sizeof(mqtt_pass));

      prefs.begin("smartsense", false);
      prefs.putString("server", mqtt_server);
      prefs.putString("port",   mqtt_port);
      prefs.putString("user",   mqtt_user);
      prefs.putString("pass",   mqtt_pass);
      prefs.end();
      Serial.println("[CONFIG] Parametri salvati in memoria!");
    }
    shouldSaveConfig = false;
  }
}

// --- LAYER 4: TRANSPORT MQTT ---
void tentativoConnessioneMqtt() {
  if (strlen(mqtt_server) == 0) return;

  String clientId = "SmartSense-Living-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  bool connected = false;
  if (strlen(mqtt_user) > 0) {
    connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, mqtt_topic_status, 1, true, "offline");
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqtt_topic_status, 1, true, "offline");
  }

  if (connected) {
    Serial.println("[MQTT] Connesso con successo!");
    mqttClient.publish(mqtt_topic_status, "online", true);
  }
}

void pubblicaDatiMQTT() {
  char tempStr[10], humStr[10], luxStr[10];
  char distStr[10], movDistStr[10], statDistStr[10], movEnStr[6], statEnStr[6];

  if (device.sht40_ok) {
    snprintf(tempStr, sizeof(tempStr), "%.2f", device.temp_c);
    snprintf(humStr,  sizeof(humStr),  "%.2f", device.hum_pct);
  } else {
    strcpy(tempStr, "null");
    strcpy(humStr, "null");
  }

  if (device.bh1750_ok) {
    snprintf(luxStr, sizeof(luxStr), "%.2f", device.lux);
  } else {
    strcpy(luxStr, "null");
  }

  if (device.radar_uart_ok) {
    snprintf(distStr,     sizeof(distStr),     "%u", device.dist_cm);
    snprintf(movDistStr,  sizeof(movDistStr),  "%u", device.moving_dist_cm);
    snprintf(statDistStr, sizeof(statDistStr), "%u", device.stat_dist_cm);
    snprintf(movEnStr,    sizeof(movEnStr),    "%u", device.moving_energy);
    snprintf(statEnStr,   sizeof(statEnStr),   "%u", device.stat_energy);
  } else {
    strcpy(distStr,     "null");
    strcpy(movDistStr,  "null");
    strcpy(statDistStr, "null");
    strcpy(movEnStr,    "null");
    strcpy(statEnStr,   "null");
  }

  char payload[512];
  snprintf(payload, sizeof(payload), 
           "{\"presence\":%s,\"radar_state\":\"%s\",\"distance_cm\":%s,"
           "\"moving_dist_cm\":%s,\"moving_energy\":%s,\"stat_dist_cm\":%s,\"stat_energy\":%s,"
           "\"temperature\":%s,\"humidity\":%s,\"lux\":%s,\"rssi\":%ld,"
           "\"diag\":{\"heap_free\":%u,\"heap_min\":%u,\"uptime_s\":%lu,\"boot\":\"%s\"},"
           "\"sensors\":{\"sht40\":\"%s\",\"bh1750\":\"%s\",\"ld2410c\":\"%s\"}}",
           device.presence_filtered ? "true" : "false",
           device.radar_uart_ok ? decodificaStatoRadar(device.target_state) : "fallback_gpio",
           distStr, movDistStr, movEnStr, statDistStr, statEnStr,
           tempStr, humStr, luxStr, (long)device.rssi,
           device.free_heap, device.min_free_heap, (unsigned long)device.uptime_sec, bootReasonStr,
           device.sht40_ok ? "ok" : "error",
           device.bh1750_ok ? "ok" : "error",
           device.radar_uart_ok ? "ok" : "error");

  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic_env, payload);
    Serial.print("[MQTT PUB] ");
    Serial.println(payload);
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); }

  bootReasonStr = ottieniMotivoReset();

  Serial.println("\n=======================================================");
  Serial.println("   SmartSense Lab - Nodo Salotto | Firmware v0.5");
  Serial.printf("   Motivo Avvio: %s\n", bootReasonStr);
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  sht4x.begin(&Wire);
  sht4x.setPrecision(SHT4X_HIGH_PRECISION);
  sht4x.setHeater(SHT4X_NO_HEATER);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

  prefs.begin("smartsense", true);
  strlcpy(mqtt_server, prefs.getString("server", "").c_str(), sizeof(mqtt_server));
  strlcpy(mqtt_port,   prefs.getString("port", "1883").c_str(), sizeof(mqtt_port));
  strlcpy(mqtt_user,   prefs.getString("user", "").c_str(), sizeof(mqtt_user));
  strlcpy(mqtt_pass,   prefs.getString("pass", "").c_str(), sizeof(mqtt_pass));
  prefs.end();

  bool mancaMqtt = (strlen(mqtt_server) == 0);
  avviaCaptivePortal(mancaMqtt);

  mqttClient.setServer(mqtt_server, atoi(mqtt_port));
  mqttClient.setBufferSize(576);

  // --- CONFIGURAZIONE WATCHDOG TIMER (WDT) ---
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true
  };
  
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);
  Serial.printf("[WDT] Task Watchdog attivo (%d secondi timeout)\n", WDT_TIMEOUT_SEC);
} // <--- parentesi di chiusura setup()

// --- LOOP PRINCIPALE ---
void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  aggiornaRadarUART();

  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      buttonPressStartTime = now;
    } else if (now - buttonPressStartTime >= PORTAL_TRIGGER_TIME) {
      Serial.println("\n[PORTAL] Richiesta apertura portale da pulsante...");
      avviaCaptivePortal(true);
      mqttClient.setServer(mqtt_server, atoi(mqtt_port));
      mqttClient.disconnect();
      isButtonPressed = false;
    }
  } else {
    isButtonPressed = false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (now - lastMqttReconnectTime >= MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnectTime = now;
        tentativoConnessioneMqtt();
      }
    } else {
      mqttClient.loop();
    }
  }

  if (device.radar_uart_ok) {
    device.presence_raw = (device.target_state > 0);
  } else {
    device.presence_raw = digitalRead(RADAR_OUT_PIN);
  }

  if (device.presence_raw) {
    lastRadarDetectionTime = now;
    device.presence_filtered = true;
  } else {
    if (now - lastRadarDetectionTime >= PRESENCE_OFF_DELAY_MS) {
      device.presence_filtered = false;
    }
  }

  bool cambioStatoPresenza = (device.presence_filtered != ultimaPresenzaTrasmessa);

  if (cambioStatoPresenza || (now - lastTelemetryTime >= TELEMETRY_INTERVAL)) {
    lastTelemetryTime = now;
    ultimaPresenzaTrasmessa = device.presence_filtered;

    leggiSensoriI2C();
    device.rssi          = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    device.free_heap     = esp_get_free_heap_size();
    device.min_free_heap = esp_get_minimum_free_heap_size();
    device.uptime_sec    = now / 1000;

    pubblicaDatiMQTT();
  }
}