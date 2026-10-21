#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>
#include <WiFiManager.h> 
#include <Preferences.h> 

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
HardwareSerial RadarSerial(1); // UART1 hardware dedicata al radar

// Flag di stato e validazione sensori I2C
bool shouldSaveConfig = false;
bool sht40_ok  = false;
bool bh1750_ok = false;

// --- Struttura Dati Radar LD2410C (UART) ---
struct RadarData {
  bool uart_valid          = false;
  unsigned long lastFrame  = 0;
  uint8_t targetState      = 0; // 0=Assente, 1=Movimento, 2=Statico, 3=Entrambi
  uint16_t movingDistCm    = 0;
  uint8_t movingEnergy     = 0;
  uint16_t statDistCm      = 0;
  uint8_t statEnergy       = 0;
  uint16_t detectionDistCm = 0;
} radar;

// Buffer per il parser binario LD2410C
uint8_t rxBuf[64];
uint8_t rxIdx = 0;

// Timer Non-Bloccanti
bool ultimaPresenza = false;
unsigned long lastTelemetryTime     = 0;
unsigned long lastMqttReconnectTime = 0;
unsigned long buttonPressStartTime  = 0;
bool isButtonPressed                = false;

constexpr unsigned long TELEMETRY_INTERVAL      = 10000; // Pubblica telemetria ogni 10s
constexpr unsigned long MQTT_RECONNECT_INTERVAL = 5000;  // Riprova MQTT ogni 5s
constexpr unsigned long PORTAL_TRIGGER_TIME     = 3000;  // 3s sul tasto BOOT per portale
constexpr unsigned long RADAR_TIMEOUT_MS        = 3000;  // Se 0 frame per 3s -> errore UART

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// --- Parser Binario Nativo per Frame HLK-LD2410C ---
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
            radar.targetState     = rxBuf[8] & 0x03;
            radar.movingDistCm    = rxBuf[9]  | (rxBuf[10] << 8);
            radar.movingEnergy    = rxBuf[11];
            radar.statDistCm      = rxBuf[12] | (rxBuf[13] << 8);
            radar.statEnergy      = rxBuf[14];
            radar.detectionDistCm = rxBuf[15] | (rxBuf[16] << 8);
            radar.lastFrame       = millis();
            radar.uart_valid      = true;
          }
        }
        rxIdx = 0; 
      }
    }
  }

  if (radar.uart_valid && (millis() - radar.lastFrame > RADAR_TIMEOUT_MS)) {
    radar.uart_valid = false;
    Serial.println("[RADAR ERRORE] Timeout comunicazione UART con LD2410C!");
  }
}

const char* decodificaStatoRadar(uint8_t state) {
  switch (state) {
    case 0:  return "none";
    case 1:  return "moving";
    case 2:  return "stationary";
    case 3:  return "both";
    default: return "unknown";
  }
}

// --- Funzione Captive Portal ---
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

  bool connesso = false;
  if (forzaApertura) {
    Serial.println("[CONFIG] Apertura manuale del Captive Portal (Rete: SmartSense-Setup)...");
    connesso = wifiManager.startConfigPortal("SmartSense-Setup");
  } else {
    Serial.println("[Wi-Fi] Connessione automatica o avvio Access Point...");
    connesso = wifiManager.autoConnect("SmartSense-Setup");
  }

  if (!connesso) {
    Serial.println("[ERRORE] Timeout portale. Riavvio scheda...");
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
      strlcpy(mqtt_port, new_port.c_str(), sizeof(mqtt_port));
      strlcpy(mqtt_user, custom_mqtt_user.getValue(), sizeof(mqtt_user));
      strlcpy(mqtt_pass, custom_mqtt_pass.getValue(), sizeof(mqtt_pass));

      prefs.begin("smartsense", false);
      prefs.putString("server", mqtt_server);
      prefs.putString("port", mqtt_port);
      prefs.putString("user", mqtt_user);
      prefs.putString("pass", mqtt_pass);
      prefs.end();
      Serial.println("[CONFIG] Nuovi parametri MQTT validati e salvati!");
    } else {
      Serial.println("[ERRORE CONFIG] Parametri non validi! Salvataggio ignorato.");
    }
    shouldSaveConfig = false;
  }
}

// --- Riconnessione MQTT Non-Bloccante ---
void tentativoConnessioneMqtt() {
  if (strlen(mqtt_server) == 0) return;

  Serial.print("[MQTT] Tentativo connessione a ");
  Serial.print(mqtt_server);
  Serial.print(" ... ");

  String clientId = "SmartSense-Living-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  bool connected = false;
  if (strlen(mqtt_user) > 0) {
    connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, mqtt_topic_status, 1, true, "offline");
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqtt_topic_status, 1, true, "offline");
  }

  if (connected) {
    Serial.println("CONNESSO!");
    mqttClient.publish(mqtt_topic_status, "online", true);
  } else {
    Serial.print("FALLITO (rc=");
    Serial.print(mqttClient.state());
    Serial.println("). Riprovo tra 5s.");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) { delay(10); }

  Serial.println("\n=======================================================");
  Serial.println("   SmartSense Lab - Nodo Salotto | Firmware v0.3");
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  // 1. Inizializzazione UART1 per HLK-LD2410C (256000 baud, 8N1, RX=GPIO17, TX=GPIO16)
  RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  Serial.println("[RADAR] UART1 inizializzata a 256000 baud (RX=D7/GPIO17, TX=D6/GPIO16)");

  // 2. Inizializzazione Sensori I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  sht40_ok = sht4x.begin(&Wire);
  if (sht40_ok) {
    sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    sht4x.setHeater(SHT4X_NO_HEATER);
    Serial.println("[SENSORI] SHT40: OK");
  } else {
    Serial.println("[SENSORI] SHT40: ERRORE");
  }

  bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  Serial.println(bh1750_ok ? "[SENSORI] BH1750: OK" : "[SENSORI] BH1750: ERRORE");

  // 3. Caricamento configurazione da NVS
  prefs.begin("smartsense", true);
  strlcpy(mqtt_server, prefs.getString("server", "").c_str(), sizeof(mqtt_server));
  strlcpy(mqtt_port,   prefs.getString("port", "1883").c_str(), sizeof(mqtt_port));
  strlcpy(mqtt_user,   prefs.getString("user", "").c_str(), sizeof(mqtt_user));
  strlcpy(mqtt_pass,   prefs.getString("pass", "").c_str(), sizeof(mqtt_pass));
  prefs.end();

  bool mancaConfigurazioneMqtt = (strlen(mqtt_server) == 0);
  avviaCaptivePortal(mancaConfigurazioneMqtt);

  Serial.print("[Wi-Fi] Operativo! IP: ");
  Serial.println(WiFi.localIP());

  mqttClient.setServer(mqtt_server, atoi(mqtt_port));
  mqttClient.setBufferSize(512);
}

void loop() {
  unsigned long now = millis();

  // --- A. Lettura Continua Stream UART Radar ---
  aggiornaRadarUART();

  // --- B. Gestione Pulsante Fisico BOOT (3s) ---
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      buttonPressStartTime = now;
    } else if (now - buttonPressStartTime >= PORTAL_TRIGGER_TIME) {
      Serial.println("\n[INPUT] Pulsante BOOT premuto per 3s -> Riapertura configurazione!");
      avviaCaptivePortal(true);
      mqttClient.setServer(mqtt_server, atoi(mqtt_port));
      mqttClient.disconnect();
      isButtonPressed = false;
    }
  } else {
    isButtonPressed = false;
  }

  // --- C. Gestione Connettività Non-Bloccante ---
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

  // --- D. Determinazione Presenza (UART Primaria + Pin OUT Fallback) ---
  bool presenzaAttuale = false;
  if (radar.uart_valid) {
    presenzaAttuale = (radar.targetState > 0);
  } else {
    presenzaAttuale = digitalRead(RADAR_OUT_PIN); 
  }

  bool cambioPresenza = (presenzaAttuale != ultimaPresenza);

  if (cambioPresenza || (now - lastTelemetryTime >= TELEMETRY_INTERVAL)) {
    lastTelemetryTime = now;
    ultimaPresenza = presenzaAttuale;

    if (cambioPresenza) {
      Serial.printf("[EVENTO RADAR] Cambio presenza -> %s (Stato: %s, Distanza: %u cm)\n",
                    presenzaAttuale ? "RILEVATA" : "ASSENTE",
                    radar.uart_valid ? decodificaStatoRadar(radar.targetState) : "fallback_gpio",
                    radar.detectionDistCm);
    }

    // 1. Lettura SHT40
    bool sht_read_valid = false;
    float t = 0.0, h = 0.0;
    if (!sht40_ok) sht40_ok = sht4x.begin(&Wire);
    if (sht40_ok) {
      sensors_event_t humidity, temp;
      if (sht4x.getEvent(&humidity, &temp)) {
        if (!isnan(temp.temperature) && temp.temperature > -40.0 && temp.temperature < 85.0 &&
            !isnan(humidity.relative_humidity) && humidity.relative_humidity >= 0.0 && humidity.relative_humidity <= 100.0) {
          t = temp.temperature;
          h = humidity.relative_humidity;
          sht_read_valid = true;
        }
      }
    }

    // 2. Lettura BH1750
    bool bh_read_valid = false;
    float lux = -1.0;
    if (!bh1750_ok) bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
    if (bh1750_ok) {
      lux = lightMeter.readLightLevel();
      if (lux >= 0.0 && lux <= 65535.0) bh_read_valid = true;
    }

    // 3. Formattazione stringhe JSON
    char tempStr[12], humStr[12], luxStr[12];
    char distStr[10], movDistStr[10], statDistStr[10], movEnStr[8], statEnStr[8];

    if (sht_read_valid) {
      snprintf(tempStr, sizeof(tempStr), "%.2f", t);
      snprintf(humStr, sizeof(humStr), "%.2f", h);
    } else {
      strcpy(tempStr, "null");
      strcpy(humStr, "null");
    }

    if (bh_read_valid) {
      snprintf(luxStr, sizeof(luxStr), "%.2f", lux);
    } else {
      strcpy(luxStr, "null");
    }

    if (radar.uart_valid) {
      snprintf(distStr,     sizeof(distStr),     "%u", radar.detectionDistCm);
      snprintf(movDistStr,  sizeof(movDistStr),  "%u", radar.movingDistCm);
      snprintf(statDistStr, sizeof(statDistStr), "%u", radar.statDistCm);
      snprintf(movEnStr,    sizeof(movEnStr),    "%u", radar.movingEnergy);
      snprintf(statEnStr,   sizeof(statEnStr),   "%u", radar.statEnergy);
    } else {
      strcpy(distStr,     "null");
      strcpy(movDistStr,  "null");
      strcpy(statDistStr, "null");
      strcpy(movEnStr,    "null");
      strcpy(statEnStr,   "null");
    }

    int32_t rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

    // 4. Costruzione Payload JSON v0.3
    char payload[480];
    snprintf(payload, sizeof(payload), 
             "{\"presence\":%s,\"radar_state\":\"%s\",\"distance_cm\":%s,"
             "\"moving_dist_cm\":%s,\"moving_energy\":%s,\"stat_dist_cm\":%s,\"stat_energy\":%s,"
             "\"temperature\":%s,\"humidity\":%s,\"lux\":%s,\"rssi\":%ld,"
             "\"sensors\":{\"sht40\":\"%s\",\"bh1750\":\"%s\",\"ld2410c\":\"%s\"}}",
             presenzaAttuale ? "true" : "false",
             radar.uart_valid ? decodificaStatoRadar(radar.targetState) : "fallback_gpio",
             distStr, movDistStr, movEnStr, statDistStr, statEnStr,
             tempStr, humStr, luxStr, (long)rssi,
             sht_read_valid ? "ok" : "error",
             bh_read_valid ? "ok" : "error",
             radar.uart_valid ? "ok" : "error");

    if (mqttClient.connected()) {
      if (mqttClient.publish(mqtt_topic_env, payload)) {
        Serial.print("[MQTT OK] ");
        Serial.println(payload);
      } else {
        Serial.println("[MQTT ERRORE] Fallita la publish()!");
      }
    } else {
      Serial.print("[LOCALE - NO MQTT] ");
      Serial.println(payload);
    }
  }
}