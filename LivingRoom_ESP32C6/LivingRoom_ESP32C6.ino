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
const char* mqtt_topic_status = "smartsense/livingroom/status"; // Per Online/Offline (LWT)

// --- Pin Hardware XIAO ESP32-C6 ---
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (GPIO3 - Uscita digitale LD2410C)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Pulsante 'B' integrato sullo XIAO per reset/config

// Oggetti globali
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Adafruit_SHT4x sht4x = Adafruit_SHT4x();
BH1750 lightMeter;
Preferences prefs;

// Flag di stato e validazione
bool shouldSaveConfig = false;
bool sht40_ok  = false;
bool bh1750_ok = false;

// Variabili operative e Timer Non-Bloccanti
bool ultimaPresenza = false;
unsigned long lastTelemetryTime     = 0;
unsigned long lastMqttReconnectTime = 0;
unsigned long buttonPressStartTime  = 0;
bool isButtonPressed                = false;

constexpr unsigned long TELEMETRY_INTERVAL      = 10000; // Pubblica telemetria ogni 10s (più pulito)
constexpr unsigned long MQTT_RECONNECT_INTERVAL = 5000;  // Riprova MQTT ogni 5s senza bloccare
constexpr unsigned long PORTAL_TRIGGER_TIME     = 3000;  // 3 secondi sul tasto BOOT per aprire il portale

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// --- Funzione per configurare e lanciare il Captive Portal ---
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

  // Stile White-Label
  String customCss = "<style>"
                     "div[style*='text-align:center;'] { display: none !important; }"
                     "button { background-color: #27AE60 !important; color: white !important; border-radius: 8px !important; }"
                     "</style>";
  wifiManager.setCustomHeadElement(customCss.c_str());
  wifiManager.setTitle("SmartSense Setup");
  wifiManager.setDarkMode(true);
  wifiManager.setConfigPortalTimeout(180); // Chiude il portale dopo 3 minuti di inattività e riavvia

  bool connesso = false;
  if (forzaApertura) {
    Serial.println("[CONFIG] Apertura manuale del Captive Portal (Rete: SmartSense-Setup)...");
    connesso = wifiManager.startConfigPortal("SmartSense-Setup");
  } else {
    Serial.println("[Wi-Fi] Connessione automatica o avvio Access Point...");
    connesso = wifiManager.autoConnect("SmartSense-Setup");
  }

  if (!connesso) {
    Serial.println("[ERRORE] Timeout o mancata connessione nel portale. Riavvio scheda...");
    delay(2000);
    ESP.restart();
  }

  // Validazione e salvataggio parametri
  if (shouldSaveConfig) {
    String new_server = custom_mqtt_server.getValue();
    String new_port   = custom_mqtt_port.getValue();
    new_server.trim();
    new_port.trim();

    int port_num = new_port.toInt();

    // Validazione di base prima di salvare
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
      Serial.println("[CONFIG] Nuovi parametri MQTT validati e salvati in memoria!");
    } else {
      Serial.println("[ERRORE CONFIG] IP Broker vuoto o Porta non valida! Salvataggio ignorato.");
    }
    shouldSaveConfig = false;
  }
}

// --- Riconnessione MQTT NON-BLOCCANTE con Last Will (LWT) ---
void tentativoConnessioneMqtt() {
  if (strlen(mqtt_server) == 0) {
    Serial.println("[MQTT] Attenzione: Nessun IP Broker configurato! Tieni premuto BOOT 3s per configurare.");
    return;
  }

  Serial.print("[MQTT] Tentativo connessione a ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqtt_port);
  Serial.print(" ... ");

  String clientId = "SmartSense-Living-";
  clientId += String( (uint32_t)ESP.getEfuseMac(), HEX ); // ID univoco basato sul MAC hardware

  bool connected = false;
  // Connessione con Last Will: se cade la linea, il broker pubblica "offline" automaticamente
  if (strlen(mqtt_user) > 0) {
    connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, mqtt_topic_status, 1, true, "offline");
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqtt_topic_status, 1, true, "offline");
  }

  if (connected) {
    Serial.println("CONNESSO!");
    // Appena connesso, annuncia a OpenHAB di essere "online" (con flag Retain = true)
    mqttClient.publish(mqtt_topic_status, "online", true);
  } else {
    Serial.print("FALLITO (rc=");
    Serial.print(mqttClient.state());
    Serial.println("). Il sistema continua a leggere i sensori, riprovo tra 5s.");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) { delay(10); }

  Serial.println("\n=======================================================");
  Serial.println("   SmartSense Lab - Nodo Salotto | Firmware v0.2");
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  // 1. Inizializzazione e Validazione Iniziale Sensori I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  
  sht40_ok = sht4x.begin(&Wire);
  if (sht40_ok) {
    sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    sht4x.setHeater(SHT4X_NO_HEATER);
    Serial.println("[SENSORI] SHT40: OK");
  } else {
    Serial.println("[SENSORI] SHT40: ERRORE (Sensore non rilevato sul bus I2C!)");
  }

  bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  if (bh1750_ok) {
    Serial.println("[SENSORI] BH1750: OK");
  } else {
    Serial.println("[SENSORI] BH1750: ERRORE (Sensore non rilevato sul bus I2C!)");
  }

  // 2. Caricamento configurazione da NVS
  prefs.begin("smartsense", true); // true = sola lettura
  strlcpy(mqtt_server, prefs.getString("server", "").c_str(), sizeof(mqtt_server));
  strlcpy(mqtt_port,   prefs.getString("port", "1883").c_str(), sizeof(mqtt_port));
  strlcpy(mqtt_user,   prefs.getString("user", "").c_str(), sizeof(mqtt_user));
  strlcpy(mqtt_pass,   prefs.getString("pass", "").c_str(), sizeof(mqtt_pass));
  prefs.end();

  // Se l'IP del broker è completamente vuoto (prima accensione assoluta), forza il portale
  bool mancaConfigurazioneMqtt = (strlen(mqtt_server) == 0);
  avviaCaptivePortal(mancaConfigurazioneMqtt);

  Serial.print("[Wi-Fi] Operativo! IP: ");
  Serial.println(WiFi.localIP());

  mqttClient.setServer(mqtt_server, atoi(mqtt_port));
  mqttClient.setBufferSize(512); // Allarga il buffer per ospitare il JSON diagnostico esteso
  
  ultimaPresenza = digitalRead(RADAR_OUT_PIN);
}

void loop() {
  unsigned long now = millis();

  // --- A. Gestione Pulsante Fisico BOOT (Tieni premuto 3s per riaprire il Captive Portal) ---
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      buttonPressStartTime = now;
    } else if (now - buttonPressStartTime >= PORTAL_TRIGGER_TIME) {
      Serial.println("\n[INPUT] Pulsante BOOT premuto per 3s -> Riapertura configurazione!");
      avviaCaptivePortal(true);
      mqttClient.setServer(mqtt_server, atoi(mqtt_port));
      mqttClient.disconnect(); // Forza riconnessione col nuovo broker
      isButtonPressed = false;
    }
  } else {
    isButtonPressed = false;
  }

  // --- B. Gestione Connettività Non-Bloccante ---
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

  // --- C. Lettura Presenza e Telemetria ---
  bool presenzaAttuale = digitalRead(RADAR_OUT_PIN);
  bool cambioPresenza  = (presenzaAttuale != ultimaPresenza);

  // Scatta IMMEDIATAMENTE se cambia la presenza, altrimenti ogni 10 secondi
  if (cambioPresenza || (now - lastTelemetryTime >= TELEMETRY_INTERVAL)) {
    lastTelemetryTime = now;
    ultimaPresenza = presenzaAttuale;

    if (cambioPresenza) {
      Serial.printf("[EVENTO RADAR] Cambio stato presenza -> %s\n", presenzaAttuale ? "RILEVATA (true)" : "ASSENTE (false)");
    }

    // 1. Lettura e Validazione SHT40
    bool sht_read_valid = false;
    float t = 0.0, h = 0.0;
    if (!sht40_ok) {
      sht40_ok = sht4x.begin(&Wire); // Tenta il recupero a caldo se si era scollegato
    }
    if (sht40_ok) {
      sensors_event_t humidity, temp;
      if (sht4x.getEvent(&humidity, &temp)) {
        // Verifica che i valori siano fisicamente sensati
        if (!isnan(temp.temperature) && temp.temperature > -40.0 && temp.temperature < 85.0 &&
            !isnan(humidity.relative_humidity) && humidity.relative_humidity >= 0.0 && humidity.relative_humidity <= 100.0) {
          t = temp.temperature;
          h = humidity.relative_humidity;
          sht_read_valid = true;
        }
      }
    }

    // 2. Lettura e Validazione BH1750
    bool bh_read_valid = false;
    float lux = -1.0;
    if (!bh1750_ok) {
      bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
    }
    if (bh1750_ok) {
      lux = lightMeter.readLightLevel();
      if (lux >= 0.0 && lux <= 65535.0) { // BH1750 restituisce -1 o -2 in caso di errore!
        bh_read_valid = true;
      }
    }

    // 3. Costruzione sicura dei valori JSON (usa "null" se un sensore è in errore, mai "0.00" falsi!)
    char tempStr[12], humStr[12], luxStr[12];
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

    int32_t rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

    // Creazione Payload JSON v0.2 con diagnostica sensori
    char payload[320];
    snprintf(payload, sizeof(payload), 
             "{\"presence\":%s,\"temperature\":%s,\"humidity\":%s,\"lux\":%s,\"rssi\":%ld,"
             "\"sensors\":{\"sht40\":\"%s\",\"bh1750\":\"%s\",\"ld2410c\":\"gpio_only\"}}",
             presenzaAttuale ? "true" : "false",
             tempStr, humStr, luxStr, (long)rssi,
             sht_read_valid ? "ok" : "error",
             bh_read_valid ? "ok" : "error");

    // 4. Pubblicazione con verifica dell'esito
    if (mqttClient.connected()) {
      if (mqttClient.publish(mqtt_topic_env, payload)) {
        Serial.print("[MQTT OK] ");
        Serial.println(payload);
      } else {
        Serial.println("[MQTT ERRORE] Fallita la publish() del messaggio!");
      }
    } else {
      Serial.print("[LOCALE - NO MQTT] ");
      Serial.println(payload);
    }
  }
}