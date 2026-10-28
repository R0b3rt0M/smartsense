#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Matter.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>

// ============================================================
// SmartSense Lab - SmartSense Room Matter Evaluation (XIAO ESP32-C6)
// Sensori: SHT40 + BH1750 + HLK-LD2410C (OUT + UART)
// Framework: Matter su Arduino ESP32 Core
// ============================================================

// -------------------- Pin Hardware XIAO ESP32-C6 --------------------
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (Trigger Rapido OUT)
constexpr uint8_t RADAR_TX_PIN    = 16; // D6 (XIAO TX -> LD2410C RX)
constexpr uint8_t RADAR_RX_PIN    = 17; // D7 (XIAO RX <- LD2410C TX)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Tasto BOOT a SINISTRA

// -------------------- Temporizzazioni -------------------------------
constexpr unsigned long SENSOR_REPORT_INTERVAL_MS = 10000UL;
constexpr unsigned long RADAR_UART_TIMEOUT_MS     = 3000UL;

// -------------------- Istanze Endpoint Matter -----------------------
MatterTemperatureSensor matterTemp;
MatterHumiditySensor    matterHum;
MatterLightSensor       matterLight;
MatterOccupancySensor   matterOccupancy;

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

// Stato Runtime
bool occupancyState = false;
unsigned long lastSensorReportTime = 0;

const char* decodificaStatoRadar(uint8_t st) {
  switch (st) {
    case 0:  return "none";
    case 1:  return "moving";
    case 2:  return "stationary";
    case 3:  return "both";
    default: return "unknown";
  }
}

// Parser UART per frame binario LD2410C
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

// Aggiornamento misure e invio agli endpoint Matter
void aggiornaMisureSensori() {
  // 1. SHT40 (Temperatura & Umidità)
  if (sht40Ok) {
    sensors_event_t h, t;
    if (sht4x.getEvent(&h, &t)) {
      if (isfinite(t.temperature) && isfinite(h.relative_humidity)) {
        matterTemp.setTemperature(t.temperature);
        matterHum.setHumidity(h.relative_humidity);
        Serial.printf("[MATTER] SHT40 -> Temp: %.2f °C | Hum: %.2f %%\n", t.temperature, h.relative_humidity);
      }
    }
  }

  // 2. BH1750 (Luce)
  if (bh1750Ok) {
    const float lux = lightMeter.readLightLevel();
    if (isfinite(lux) && lux >= 0.0f) {
      matterLight.setIlluminance(lux);
      Serial.printf("[MATTER] BH1750 -> Luce: %.2f lx\n", lux);
    }
  }

  // 3. Radar LD2410C Distanza (Stampa diagnostica)
  if (radar.uart_ok) {
    Serial.printf("[RADAR UART] Stato: %s | Distanza: %u cm\n",
                  decodificaStatoRadar(radar.target_state), radar.dist_cm);
  }
}

// Gestione Presenza da pin OUT
void aggiornaPresenza() {
  const bool nuovaPresenza = (digitalRead(RADAR_OUT_PIN) == HIGH);
  if (nuovaPresenza != occupancyState) {
    occupancyState = nuovaPresenza;
    matterOccupancy.setOccupancy(occupancyState);
    Serial.printf("[MATTER] Presenza -> %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=======================================================");
  Serial.println(" SmartSense Room - Matter (XIAO ESP32-C6)");
  Serial.println(" Sensori: SHT40 | BH1750 | LD2410C (UART+OUT)");
  Serial.println("=======================================================");

  pinMode(RADAR_OUT_PIN, INPUT);
  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  // Inizializzazione UART1 per radar LD2410C
  RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  // Inizializzazione I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  sht40Ok  = sht4x.begin(&Wire);
  if (sht40Ok) {
    sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    sht4x.setHeater(SHT4X_NO_HEATER);
  }
  bh1750Ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

  Serial.printf("[SENSORI] SHT40: %s | BH1750: %s\n", sht40Ok ? "OK" : "ERR", bh1750Ok ? "OK" : "ERR");

  // Inizializzazione degli endpoint Matter
  matterTemp.begin(20.0);
  matterHum.begin(50.0);
  matterLight.begin(100.0);
  matterOccupancy.begin(false);

  // Avvio dello stack Matter (avvia BLE + Commissioning)
  Matter.begin();

  Serial.println("[MATTER] Stack avviato.");
  
  if (!Matter.isDeviceCommissioned()) {
    Serial.println("\n-------------------------------------------------------");
    Serial.println(">>> DISPOSITIVO NON ASSOCIATO: PRONTO PER IL PAIRING <<<");
    Serial.println("Apri l'app Google Home sul telefono -> Aggiungi dispositivo Matter");
    Serial.println("-------------------------------------------------------\n");
  } else {
    Serial.println("\n>>> DISPOSITIVO GIA' ASSOCIATO AL FABRIC MATTER <<<\n");
  }

  lastSensorReportTime = millis();
}

void loop() {
  const unsigned long now = millis();

  // 1. Parsing continuo dati radar via UART
  aggiornaRadarUART();

  // 2. Controllo rapido presenza pin OUT
  aggiornaPresenza();

  // 3. Invio periodico misure ambientali agli endpoint Matter
  if (now - lastSensorReportTime >= SENSOR_REPORT_INTERVAL_MS) {
    lastSensorReportTime = now;
    aggiornaMisureSensori();
  }

  // 4. Factory reset Matter tenendo premuto BOOT per 10 secondi
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_BOOT_PIN) == LOW) {
      delay(50);
      if (millis() - pressStart > 10000UL) {
        Serial.println("\n[RESET] Decommissioning Matter in corso...");
        Matter.decommission();
        delay(1000);
        ESP.restart();
      }
    }
  }

  delay(10);
}