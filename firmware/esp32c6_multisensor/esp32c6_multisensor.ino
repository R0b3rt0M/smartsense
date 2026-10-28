#include <Arduino.h>
#include <Matter.h>
#include "sensors_manager.h"

// ============================================================
// SmartSense Lab - Refactoring Test (HAL + Matter)
// Hardware: Seeed XIAO ESP32-C6
// Branch: refactor/sensor-abstraction
// ============================================================

// -------------------- Assegnazione Pin XIAO ESP32-C6 ----------------
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (Trigger rapido presenza)
constexpr uint8_t RADAR_TX_PIN    = 16; // D6 (XIAO TX -> LD2410C RX)
constexpr uint8_t RADAR_RX_PIN    = 17; // D7 (XIAO RX <- LD2410C TX)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Tasto BOOT a SINISTRA

// -------------------- Temporizzazioni -------------------------------
constexpr unsigned long SENSOR_REPORT_INTERVAL_MS = 10000UL;

// -------------------- Oggetti Globali -------------------------------
SensorsManager sensors(I2C_SDA_PIN, I2C_SCL_PIN, RADAR_OUT_PIN, RADAR_RX_PIN, RADAR_TX_PIN);

MatterTemperatureSensor matterTemp;
MatterHumiditySensor    matterHum;
MatterLightSensor       matterLight;
MatterOccupancySensor   matterOccupancy;

bool lastOccupancyState = false;
unsigned long lastReportTime = 0;

void inviaMisureSensori() {
  float temp = 0.0f;
  float hum  = 0.0f;
  float lux  = 0.0f;
  RadarData radar;

  // 1. SHT40 (Temperatura e Umidità)
  if (sensors.getTemperature(temp) && sensors.getHumidity(hum)) {
    matterTemp.setTemperature(temp);
    matterHum.setHumidity(hum);
    Serial.printf("[HAL -> MATTER] Temp: %.2f °C | Hum: %.2f %%\n", temp, hum);
  } else {
    Serial.println("[HAL] SHT40 lettura non valida");
  }

  // 2. BH1750 (Luce in lux)
  if (sensors.getIlluminance(lux)) {
    matterLight.setIlluminance(lux);
    Serial.printf("[HAL -> MATTER] Luce: %.2f lx\n", lux);
  } else {
    Serial.println("[HAL] BH1750 lettura non valida");
  }

  // 3. Radar LD2410C (Distanza e diagnostica)
  if (sensors.getRadarData(radar)) {
    Serial.printf("[HAL -> RADAR] Stato: %s | Dist: %u cm (Mov: %u cm | Stat: %u cm)\n",
                  sensors.getRadarStateString(), radar.dist_cm, radar.moving_dist, radar.stat_dist);
  }
}

void aggiornaPresenza() {
  bool currentOccupancy = false;
  sensors.getOccupancy(currentOccupancy);

  if (currentOccupancy != lastOccupancyState) {
    lastOccupancyState = currentOccupancy;
    matterOccupancy.setOccupancy(lastOccupancyState);
    Serial.printf("[HAL -> MATTER] Presenza: %s\n", lastOccupancyState ? "RILEVATA" : "ASSENTE");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=======================================================");
  Serial.println(" SmartSense Lab - Test HAL con Stack Matter");
  Serial.println("=======================================================");

  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  // Inizializzazione modulo sensori
  if (sensors.begin()) {
    Serial.println("[HAL] Sensori I2C inizializzati con successo.");
  } else {
    Serial.println("[HAL] Attenzione: uno o piu sensori I2C non rilevati.");
  }

  // Inizializzazione endpoint Matter
  matterTemp.begin(20.0);
  matterHum.begin(50.0);
  matterLight.begin(100.0);
  matterOccupancy.begin(false);

  Matter.begin();
  Serial.println("[MATTER] Stack avviato.");

  if (!Matter.isDeviceCommissioned()) {
    Serial.println("[MATTER] Dispositivo pronto per il commissioning.");
  } else {
    Serial.println("[MATTER] Dispositivo gia associato alla rete.");
  }

  lastReportTime = millis();
}

void loop() {
  const unsigned long now = millis();

  // 1. Aggiornamento continuo macchina a stati sensori
  sensors.update();

  // 2. Trigger presenza
  aggiornaPresenza();

  // 3. Invio periodico letture
  if (now - lastReportTime >= SENSOR_REPORT_INTERVAL_MS) {
    lastReportTime = now;
    inviaMisureSensori();
  }

  // 4. Reset Matter con pressione prolungata (10 secondi)
  if (digitalRead(BUTTON_BOOT_PIN) == LOW) {
    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_BOOT_PIN) == LOW) {
      delay(50);
      if (millis() - pressStart > 10000UL) {
        Serial.println("\n[RESET] Decommissioning Matter...");
        Matter.decommission();
        delay(1000);
        ESP.restart();
      }
    }
  }

  delay(10);
}