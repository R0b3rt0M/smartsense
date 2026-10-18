#include <WiFi.h>
#include <PubSubClient.h>

// --- Credenziali Rete Wi-Fi (Banda 2.4 GHz) ---
const char* wifi_ssid     = "Il_Tuo_SSID";       // Inserisci il nome della tua rete a 2.4 GHz
const char* wifi_password = "La_Tua_Password";   // Inserisci la password

// --- Configurazione Broker MQTT ---
const char* mqtt_server   = "192.168.1.174";     // IP del tuo broker MQTT
const int   mqtt_port     = 1883;                // Porta (default 1883)
const char* mqtt_user     = "";                  // Nome utente (lascia "" se non richiesto)
const char* mqtt_pass     = "";                  // Password (lascia "" se non richiesto)

WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long lastStatusPrint = 0;
const unsigned long statusInterval = 5000;

void setupWifi() {
  delay(10);
  Serial.println();
  Serial.println("========================================");
  Serial.print("[Wi-Fi] Tentativo di connessione a: ");
  Serial.println(wifi_ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("[Wi-Fi] Connessione stabilita con successo!");
  Serial.print("[Wi-Fi] Indirizzo IP assegnato: ");
  Serial.println(WiFi.localIP());
  Serial.print("[Wi-Fi] RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  Serial.println("========================================");
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Tentativo di connessione al broker...");
    
    String clientId = "XIAO-C6-Test-";
    clientId += String(random(0xffff), HEX);

    bool isConnected = false;
    if (strlen(mqtt_user) > 0) {
      isConnected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    } else {
      isConnected = mqttClient.connect(clientId.c_str());
    }

    if (isConnected) {
      Serial.println(" CONNESSO!");
      Serial.println("[MQTT] Connessione riuscita.");
      mqttClient.publish("smartsense/test/status", "online_test_step1");
      Serial.println("[MQTT] Inviato messaggio di test su 'smartsense/test/status'");
    } else {
      Serial.print(" FALLITO, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" | Riprovo tra 3 secondi...");
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Attende l'apertura del Monitor Seriale USB CDC (max 4 sec)
  while (!Serial && millis() < 4000) {
    ;
  }

  Serial.println("\n--- SMART SENSE LAB: Step 1 (MCU Nuda) ---");
  Serial.println("[SYSTEM] XIAO ESP32-C6 Avviata");

  setupWifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Wi-Fi] Connessione persa. Riconnessione...");
    setupWifi();
  }

  if (!mqttClient.connected()) {
    reconnectMqtt();
  }
  
  mqttClient.loop();

  if (millis() - lastStatusPrint >= statusInterval) {
    lastStatusPrint = millis();
    Serial.println("[HEARTBEAT] Wi-Fi OK | MQTT OK | Stack di rete operativo");
  }
}