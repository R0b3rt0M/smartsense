#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Zigbee.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>

// ============================================================
// SmartSense Lab - Zigbee Multisensore v0.4.1 (UART Radar)
// Hardware: Seeed XIAO ESP32-C6
// Sensori: SHT40 + BH1750 + HLK-LD2410C (OUT + UART)
// Ruolo Zigbee: Router (IEEE 802.15.4)
// ============================================================

// -------------------- Pin Hardware XIAO ESP32-C6 --------------------
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (Trigger Rapido)
constexpr uint8_t RADAR_TX_PIN    = 16; // D6 (XIAO TX -> LD2410C RX)
constexpr uint8_t RADAR_RX_PIN    = 17; // D7 (XIAO RX <- LD2410C TX)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Pulsante 'B' integrato (Attivo LOW)

// -------------------- Endpoint Zigbee Canonici ----------------------
constexpr uint8_t ZIGBEE_EP_OCCUPANCY   = 10; // Presenza binaria (0x0406)
constexpr uint8_t ZIGBEE_EP_ENVIRONMENT = 11; // Temp (0x0402) + Umidità (0x0405)
constexpr uint8_t ZIGBEE_EP_ILLUMINANCE  = 12; // Luce ZCL (0x0400)
constexpr uint8_t ZIGBEE_EP_DISTANCE    = 14; // Distanza Radar cm (Analog 0x000C)

// -------------------- Temporizzazioni e Debounce ---------------------
constexpr unsigned long SENSOR_REPORT_INTERVAL_MS = 10000UL;
constexpr unsigned long FACTORY_RESET_HOLD_MS     = 3000UL;
constexpr unsigned long BUTTON_DEBOUNCE_MS        = 50UL;
constexpr unsigned long RADAR_UART_TIMEOUT_MS     = 3000UL;

// -------------------- Istanze Oggetti Zigbee ------------------------
ZigbeeOccupancySensor   zbOccupancy(ZIGBEE_EP_OCCUPANCY);
ZigbeeTempSensor        zbEnvironment(ZIGBEE_EP_ENVIRONMENT);
ZigbeeIlluminanceSensor zbIlluminance(ZIGBEE_EP_ILLUMINANCE);
ZigbeeAnalog            zbDistance(ZIGBEE_EP_DISTANCE);

// -------------------- Sensori Fisici --------------------------------
Adafruit_SHT4x sht4x;
BH1750 lightMeter;
HardwareSerial RadarSerial(1);

bool sht40Ok  = false;
bool bh1750Ok = false;

// -------------------- Parser & Stato LD2410C UART -------------------
struct RadarData {
  bool uart_ok          = false;
  uint8_t target_state  = 0; // 0=none, 1=moving, 2=stat, 3=both
  uint16_t dist_cm      = 0;
  uint16_t moving_dist  = 0;
  uint8_t  moving_energy= 0;
  uint16_t stat_dist    = 0;
  uint8_t  stat_energy  = 0;
} radar;

uint8_t rxBuf[64];
uint8_t rxIdx = 0;
unsigned long lastRadarFrameTime = 0;

// -------------------- Stato Runtime ---------------------------------
bool occupancyState        = false;
bool lastConnectedState    = false;

unsigned long lastSensorReportTime = 0;
unsigned long buttonPressTime      = 0;
unsigned long buttonLastChangeTime = 0;

bool buttonWasLow          = false;
bool factoryResetTriggered = false;

// Conversione ZCL Lux logaritmica (Cluster 0x0400)
uint16_t luxToZclMeasuredValue(float lux) {
  if (!isfinite(lux) || lux <= 0.0001f) return 0;
  if (lux > 100000.0f) lux = 100000.0f;
  float zcl = 10000.0f * log10f(lux) + 1.0f;
  if (zcl < 1.0f) zcl = 1.0f;
  if (zcl > 0xFFFE) zcl = 0xFFFE;
  return static_cast<uint16_t>(lroundf(zcl));
}

const char* decodificaStatoRadar(uint8_t st) {
  switch (st) {
    case 0:  return "none";
    case 1:  return "moving";
    case 2:  return "stationary";
    case 3:  return "both";
    default: return "unknown";
  }
}

// ------------------------------------------------------------
// Parser UART per frame binario LD2410C
// ------------------------------------------------------------
void aggiornaRadarUART() {
  while (RadarSerial.available()) {
    uint8_t b = RadarSerial.read();

    if (rxIdx < sizeof(rxBuf)) {
      rxBuf[rxIdx++] = b;
    } else {
      rxIdx = 0;
    }

    // Ricerca Header standard LD2410 (F4 F3 F2 F1)
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
        // Verifica trailer standard LD2410 (F8 F7 F6 F5)
        if (rxBuf[totalFrameLen - 4] == 0xF8 &&
            rxBuf[totalFrameLen - 3] == 0xF7 &&
            rxBuf[totalFrameLen - 2] == 0xF6 &&
            rxBuf[totalFrameLen - 1] == 0xF5) {
          
          if ((rxBuf[6] == 0x02 || rxBuf[6] == 0x01) && rxBuf[7] == 0xAA) {
            radar.target_state  = rxBuf[8] & 0x03;
            radar.moving_dist   = rxBuf[9]  | (rxBuf[10] << 8);
            radar.moving_energy = rxBuf[11];
            radar.stat_dist     = rxBuf[12] | (rxBuf[13] << 8);
            radar.stat_energy   = rxBuf[14];
            radar.dist_cm       = rxBuf[15] | (rxBuf[16] << 8);
            
            lastRadarFrameTime  = millis();
            radar.uart_ok       = true;
          }
        }
        rxIdx = 0;
      }
    }
  }

  if (radar.uart_ok && (millis() - lastRadarFrameTime > RADAR_UART_TIMEOUT_MS)) {
    radar.uart_ok = false;
  }
}

// ------------------------------------------------------------
// Report periodico sensori (I2C + UART Distanza)
// ------------------------------------------------------------
void inviaMisureSensori() {
  // 1. SHT40
  if (sht40Ok) {
    sensors_event_t h, t;
    if (sht4x.getEvent(&h, &t)) {
      if (isfinite(t.temperature) && isfinite(h.relative_humidity)) {
        zbEnvironment.setTemperature(t.temperature);
        zbEnvironment.setHumidity(h.relative_humidity);
        zbEnvironment.report();
        Serial.printf("[ZIGBEE] SHT40 -> Temp: %.2f °C | Hum: %.2f %%\n", t.temperature, h.relative_humidity);
      }
    }
  }

  // 2. BH1750
  if (bh1750Ok) {
    const float lux = lightMeter.readLightLevel();
    if (isfinite(lux) && lux >= 0.0f) {
      const uint16_t zclValue = luxToZclMeasuredValue(lux);
      zbIlluminance.setIlluminance(zclValue);
      zbIlluminance.report();
      Serial.printf("[ZIGBEE] BH1750 -> Luce: %.2f lx (ZCL: %u)\n", lux, zclValue);
    }
  }

  // 3. Radar mmWave UART Distanza (Analog Input Cluster 0x000C)
  // ZigbeeAnalog usa setAnalogInput() e reportAnalogInput().
  if (radar.uart_ok) {
    const float distanceCm = (float)radar.dist_cm;

    if (zbDistance.setAnalogInput(distanceCm)) {
      if (zbDistance.reportAnalogInput()) {
        Serial.printf("[ZIGBEE] LD2410C UART -> Stato: %s | Dist: %.0f cm "
                      "(Mov: %u cm, En: %u | Stat: %u cm, En: %u)\n",
                      decodificaStatoRadar(radar.target_state), distanceCm,
                      radar.moving_dist, radar.moving_energy,
                      radar.stat_dist, radar.stat_energy);
      } else {
        Serial.println("[ZIGBEE] Avviso: report distanza radar non riuscito.");
      }
    } else {
      Serial.println("[ZIGBEE] Avviso: aggiornamento valore distanza radar non riuscito.");
    }
  }
}

// ------------------------------------------------------------
// Gestione Presenza (Trigger rapido da pin OUT)
// ------------------------------------------------------------
void aggiornaPresenza(bool forzaReport = false) {
  const bool nuovaPresenza = (digitalRead(RADAR_OUT_PIN) == HIGH);
  const bool cambiata      = (nuovaPresenza != occupancyState);

  if (cambiata) {
    occupancyState = nuovaPresenza;
    Serial.printf("[RADAR] Presenza -> %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
  }

  if (cambiata || forzaReport) {
    zbOccupancy.setOccupancy(occupancyState);
    if (Zigbee.connected()) {
      zbOccupancy.report();
      Serial.printf("[ZIGBEE] Report presenza trasmesso: %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
    }
  }
}

// ------------------------------------------------------------
// Gestione Pulsante BOOT (Factory Reset Zigbee)
// ------------------------------------------------------------
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

  if (buttonLow &&
      buttonWasLow &&
      !factoryResetTriggered &&
      (now - buttonLastChangeTime >= BUTTON_DEBOUNCE_MS) &&
      (now - buttonPressTime >= FACTORY_RESET_HOLD_MS)) {

    factoryResetTriggered = true;
    Serial.println("\n[RESET] Factory reset Zigbee richiesto...");
    Serial.println("[RESET] Azzeramento credenziali Flash NVS...");
    Zigbee.factoryReset();
    delay(1000);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  const unsigned long serialStart = millis();
  while (!Serial && millis() - serialStart < 3000UL) { delay(10); }

  Serial.println("\n=======================================================");
  Serial.println(" SmartSense Lab - Zigbee Multisensore v0.4.1 (UART Radar)");
  Serial.println(" XIAO ESP32-C6 | SHT40 | BH1750 | LD2410C (UART+OUT)");
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  // Inizializzazione UART per radar LD2410C (D6 TX, D7 RX)[cite: 2]
  RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  // Inizializzazione I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); // SDA GPIO22, SCL GPIO23[cite: 1, 2]
  sht40Ok  = sht4x.begin(&Wire);
  if (sht40Ok) {
    sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    sht4x.setHeater(SHT4X_NO_HEATER);
  }
  bh1750Ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

  Serial.printf("[SENSORI] SHT40: %s | BH1750: %s\n", sht40Ok ? "OK" : "ERR", bh1750Ok ? "OK" : "ERR");

  // Configurazione Endpoint Zigbee
  zbEnvironment.addHumiditySensor(0.0f, 100.0f, 0.1f);
  zbIlluminance.setMinMaxValue(0, 0xFFFE);

  // ZigbeeAnalog non crea automaticamente il cluster Analog Input:
  // va aggiunto esplicitamente prima di registrare l'endpoint.
  zbDistance.addAnalogInput();
  zbDistance.setAnalogInputDescription("LD2410C distance (cm)");
  zbDistance.setAnalogInputResolution(1.0f);
  zbDistance.setAnalogInputMinMax(0.0f, 600.0f);

  // Impostazione Identità ZCL
  zbOccupancy.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");
  zbEnvironment.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");
  zbIlluminance.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");
  zbDistance.setManufacturerAndModel("SmartSense", "MultiSensor-ZR");

  // Registrazione Endpoint
  Zigbee.addEndpoint(&zbOccupancy);
  Zigbee.addEndpoint(&zbEnvironment);
  Zigbee.addEndpoint(&zbIlluminance);
  Zigbee.addEndpoint(&zbDistance);

  Serial.println("[ZIGBEE] Avvio router...");
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("[ERRORE] Inizializzazione fallita!");
    delay(2000);
    ESP.restart();
  }

  Serial.println("[ZIGBEE] Stack operativo.");
  lastSensorReportTime = millis();
}

void loop() {
  const unsigned long now = millis();

  // 1. Parsing continuo dati radar via UART
  aggiornaRadarUART();

  // 2. Controllo Pulsante Reset
  gestisciPulsante(now);

  // 3. Monitoraggio Connessione Mesh
  const bool isConnected = Zigbee.connected();
  if (isConnected != lastConnectedState) {
    lastConnectedState = isConnected;
    if (isConnected) {
      Serial.println("\n>>> [ZIGBEE] CONNESSO ALLA RETE MESH <<<");
      aggiornaPresenza(true);
      inviaMisureSensori();
      lastSensorReportTime = millis();
    }
  }

  // 4. Trigger immediato presenza da pin OUT
  aggiornaPresenza(false);

  // 5. Invio periodico misure ambientali e distanza radar
  if (now - lastSensorReportTime >= SENSOR_REPORT_INTERVAL_MS) {
    lastSensorReportTime = now;
    if (isConnected) {
      inviaMisureSensori();
    }
  }

  delay(10);
}