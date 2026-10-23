#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Zigbee.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>

// ============================================================
// SmartSense Lab - Zigbee Multisensore v0.3.1
// Hardware: Seeed XIAO ESP32-C6
// Sensori: SHT40 + BH1750 + HLK-LD2410C
// Ruolo Zigbee: Router (Alimentato a rete)
// ============================================================

// -------------------- Pin Hardware XIAO ESP32-C6 --------------------
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Pulsante 'B' integrato (Attivo LOW)

// -------------------- Endpoint Zigbee Canonici ----------------------
constexpr uint8_t ZIGBEE_EP_OCCUPANCY   = 10;
constexpr uint8_t ZIGBEE_EP_ENVIRONMENT = 11; // Temperatura (0x0402) + Umidità (0x0405)
constexpr uint8_t ZIGBEE_EP_ILLUMINANCE  = 12; // Illuminamento (0x0400)

// -------------------- Temporizzazioni e Debounce ---------------------
constexpr unsigned long SENSOR_REPORT_INTERVAL_MS = 10000UL;
constexpr unsigned long FACTORY_RESET_HOLD_MS     = 3000UL;
constexpr unsigned long BUTTON_DEBOUNCE_MS        = 50UL;

// -------------------- Istanze Oggetti Zigbee ------------------------
ZigbeeOccupancySensor   zbOccupancy(ZIGBEE_EP_OCCUPANCY);
ZigbeeTempSensor        zbEnvironment(ZIGBEE_EP_ENVIRONMENT);
ZigbeeIlluminanceSensor zbIlluminance(ZIGBEE_EP_ILLUMINANCE);

// -------------------- Sensori Fisici --------------------------------
Adafruit_SHT4x sht4x;
BH1750 lightMeter;

bool sht40Ok  = false;
bool bh1750Ok = false;

// -------------------- Stato Runtime ---------------------------------
bool occupancyState        = false;
bool lastConnectedState    = false;

unsigned long lastSensorReportTime = 0;
unsigned long buttonPressTime      = 0;
unsigned long buttonLastChangeTime = 0;

bool buttonWasLow          = false;
bool factoryResetTriggered = false;

// ============================================================
// Conversione ZCL: Lux -> MeasuredValue (Cluster 0x0400)
// Formula ZCL: MeasuredValue = 10000 * log10(lux) + 1
// ============================================================
uint16_t luxToZclMeasuredValue(float lux) {
  if (!isfinite(lux) || lux <= 0.0001f) {
    return 0; // Standard ZCL: troppo buio / limite inferiore
  }
  if (lux > 100000.0f) {
    lux = 100000.0f;
  }
  float zcl = 10000.0f * log10f(lux) + 1.0f;
  if (zcl < 1.0f) zcl = 1.0f;
  if (zcl > 0xFFFE) zcl = 0xFFFE;
  return static_cast<uint16_t>(lroundf(zcl));
}

// ============================================================
// Lettura e Report Misure Ambientali
// ============================================================
void inviaMisureSensori() {
  // 1. Temperatura e Umidità (SHT40)
  if (sht40Ok) {
    sensors_event_t humidityEvent;
    sensors_event_t temperatureEvent;

    if (sht4x.getEvent(&humidityEvent, &temperatureEvent)) {
      const float temperature = temperatureEvent.temperature;
      const float humidity    = humidityEvent.relative_humidity;

      if (isfinite(temperature) && isfinite(humidity) &&
          temperature >= -40.0f && temperature <= 125.0f &&
          humidity >= 0.0f && humidity <= 100.0f) {

        zbEnvironment.setTemperature(temperature);
        zbEnvironment.setHumidity(humidity);

        if (zbEnvironment.report()) {
          Serial.printf("[ZIGBEE] SHT40 -> Temp: %.2f °C | Umidita: %.2f %%\n", temperature, humidity);
        } else {
          Serial.println("[ZIGBEE] Errore report SHT40");
        }
      } else {
        Serial.println("[SHT40] Dati letti fuori range");
      }
    } else {
      Serial.println("[SHT40] Errore lettura bus I2C");
    }
  }

  // 2. Illuminamento (BH1750)
  if (bh1750Ok) {
    const float lux = lightMeter.readLightLevel();

    if (isfinite(lux) && lux >= 0.0f && lux <= 65535.0f) {
      const uint16_t zclValue = luxToZclMeasuredValue(lux);

      zbIlluminance.setIlluminance(zclValue);

      if (zbIlluminance.report()) {
        Serial.printf("[ZIGBEE] BH1750 -> Luce: %.2f lx (ZCL Raw: %u)\n", lux, zclValue);
      } else {
        Serial.println("[ZIGBEE] Errore report BH1750");
      }
    } else {
      Serial.printf("[BH1750] Lettura non valida: %.2f\n", lux);
    }
  }
}

// ============================================================
// Aggiornamento Presenza (LD2410C pin OUT)
// ============================================================
void aggiornaPresenza(bool forzaReport = false) {
  const bool nuovaPresenza = (digitalRead(RADAR_OUT_PIN) == HIGH);
  const bool cambiata      = (nuovaPresenza != occupancyState);

  if (cambiata) {
    occupancyState = nuovaPresenza;
    Serial.printf("[RADAR] Presenza: %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
  }

  if (cambiata || forzaReport) {
    zbOccupancy.setOccupancy(occupancyState);

    if (Zigbee.connected()) {
      if (zbOccupancy.report()) {
        Serial.printf("[ZIGBEE] Report presenza trasmesso: %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
      } else {
        Serial.println("[ZIGBEE] Errore report presenza");
      }
    }
  }
}

// ============================================================
// Gestione Pressione Pulsante BOOT (Factory Reset Zigbee)
// ============================================================
void gestisciPulsante(unsigned long now) {
  const bool buttonLow = (digitalRead(BUTTON_BOOT_PIN) == LOW);

  if (buttonLow && !buttonWasLow) {
    buttonPressTime       = now;
    buttonLastChangeTime  = now;
    buttonWasLow          = true;
    factoryResetTriggered = false;
  }

  if (!buttonLow && buttonWasLow) {
    buttonWasLow          = false;
    factoryResetTriggered = false;
  }

  if (buttonLow && buttonWasLow && !factoryResetTriggered &&
      (now - buttonLastChangeTime >= BUTTON_DEBOUNCE_MS) &&
      (now - buttonPressTime >= FACTORY_RESET_HOLD_MS)) {

    factoryResetTriggered = true;

    Serial.println("\n[RESET] Factory reset Zigbee richiesto...");
    Serial.println("[RESET] Azzeramento credenziali Flash NVS...");

    Zigbee.factoryReset(true);

    while (true) {
      delay(1000);
    }
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);

  const unsigned long serialStart = millis();
  while (!Serial && millis() - serialStart < 3000UL) {
    delay(10);
  }

  Serial.println("\n=======================================================");
  Serial.println(" SmartSense Lab - Zigbee Multisensore v0.3.1");
  Serial.println(" XIAO ESP32-C6 | SHT40 | BH1750 | LD2410C");
  Serial.println(" Ruolo: Zigbee Router (IEEE 802.15.4)");
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  occupancyState = (digitalRead(RADAR_OUT_PIN) == HIGH);

  // 1. Inizializzazione Bus I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 2. SHT40
  sht40Ok = sht4x.begin(&Wire);
  if (sht40Ok) {
    sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    sht4x.setHeater(SHT4X_NO_HEATER);
  }

  // 3. BH1750
  bh1750Ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

  Serial.printf("[SENSORI] SHT40: %s | BH1750: %s\n", 
                sht40Ok ? "OK" : "ERRORE", 
                bh1750Ok ? "OK" : "ERRORE");

  // 4. Configurazione Endpoint ZCL
  zbEnvironment.addHumiditySensor(0.0f, 100.0f, 0.1f);
  zbIlluminance.setMinMaxValue(0, 0xFFFE);

  zbOccupancy.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");
  zbEnvironment.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");
  zbIlluminance.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");

  zbOccupancy.setOccupancy(occupancyState);

  // 5. Registrazione Endpoint
  if (!Zigbee.addEndpoint(&zbOccupancy)) {
    Serial.println("[ERRORE] Registrazione endpoint presenza fallita");
    delay(2000);
    ESP.restart();
  }

  if (!Zigbee.addEndpoint(&zbEnvironment)) {
    Serial.println("[ERRORE] Registrazione endpoint ambientale fallita");
    delay(2000);
    ESP.restart();
  }

  if (!Zigbee.addEndpoint(&zbIlluminance)) {
    Serial.println("[ERRORE] Registrazione endpoint illuminamento fallita");
    delay(2000);
    ESP.restart();
  }

  // 6. Avvio Stack Router
  Serial.println("[ZIGBEE] Avvio stack radio in modalita' Router...");
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("[ERRORE] Avvio stack Zigbee fallito! Riavvio...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("[ZIGBEE] Stack operativo. In ascolto sulla mesh...");
  lastSensorReportTime = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  const unsigned long now = millis();

  // 1. Controllo pulsante BOOT
  gestisciPulsante(now);

  // 2. Monitoraggio Connessione Mesh
  const bool isConnected = Zigbee.connected();
  if (isConnected != lastConnectedState) {
    lastConnectedState = isConnected;

    if (isConnected) {
      Serial.println("\n>>> [ZIGBEE] CONNESSO ALLA RETE MESH <<<");
      aggiornaPresenza(true);
      inviaMisureSensori();
      lastSensorReportTime = millis();
    } else {
      Serial.println("[ZIGBEE] Disconnesso o in ricerca di sincronizzazione mesh");
    }
  }

  // 3. Monitoraggio rapido presenza radar
  aggiornaPresenza(false);

  // 4. Invio periodico misure ambientali
  if (now - lastSensorReportTime >= SENSOR_REPORT_INTERVAL_MS) {
    lastSensorReportTime = now;

    if (isConnected) {
      inviaMisureSensori();
    } else {
      // Diagnostica offline
      if (sht40Ok) {
        sensors_event_t h, t;
        if (sht4x.getEvent(&h, &t)) {
          Serial.printf("[OFFLINE] Temp: %.2f °C | Hum: %.2f %%\n", t.temperature, h.relative_humidity);
        }
      }
      if (bh1750Ok) {
        Serial.printf("[OFFLINE] Luce: %.2f lx\n", lightMeter.readLightLevel());
      }
    }
  }

  delay(20);
}