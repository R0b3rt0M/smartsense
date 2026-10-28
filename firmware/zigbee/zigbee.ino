#include <Arduino.h>
#include <Zigbee.h>
#include "sensors_manager.h"

// ============================================================
// SmartSense Lab - SmartSense Room Zigbee v1.0 (Router + OTA)
// Hardware: Seeed XIAO ESP32-C6
// Sensori: SHT40 + BH1750 + HLK-LD2410C (UART + OUT)
// ============================================================

// -------------------- Identità ZCL & Parametri OTA -------------------
constexpr uint16_t SMARTSENSE_MANUFACTURER_CODE = 0x1234;
constexpr uint16_t SMARTSENSE_IMAGE_TYPE        = 0x0001;
constexpr uint32_t SMARTSENSE_CURRENT_VERSION   = 0x00000001; // v1.0.0
constexpr uint16_t SMARTSENSE_HW_VERSION        = 1;

// -------------------- Pin Hardware XIAO ESP32-C6 --------------------
constexpr uint8_t I2C_SDA_PIN     = 22; // D4
constexpr uint8_t I2C_SCL_PIN     = 23; // D5
constexpr uint8_t RADAR_OUT_PIN   = 3;  // D1 (Trigger rapido OUT)
constexpr uint8_t RADAR_TX_PIN    = 16; // D6 (XIAO TX -> LD2410C RX)
constexpr uint8_t RADAR_RX_PIN    = 17; // D7 (XIAO RX <- LD2410C TX)
constexpr uint8_t BUTTON_BOOT_PIN = 9;  // Tasto BOOT (Attivo LOW)

// -------------------- Endpoint Zigbee Canonici ----------------------
constexpr uint8_t ZIGBEE_EP_OCCUPANCY   = 10;
constexpr uint8_t ZIGBEE_EP_ENVIRONMENT = 11;
constexpr uint8_t ZIGBEE_EP_ILLUMINANCE = 12;
constexpr uint8_t ZIGBEE_EP_DISTANCE    = 14;

// -------------------- Temporizzazioni e Debounce ---------------------
constexpr unsigned long SENSOR_REPORT_INTERVAL_MS = 10000UL;
constexpr unsigned long FACTORY_RESET_HOLD_MS     = 3000UL;
constexpr unsigned long BUTTON_DEBOUNCE_MS        = 50UL;

// -------------------- Oggetti Globali -------------------------------
SensorsManager sensors(I2C_SDA_PIN, I2C_SCL_PIN, RADAR_OUT_PIN, RADAR_RX_PIN, RADAR_TX_PIN);

ZigbeeOccupancySensor   zbOccupancy(ZIGBEE_EP_OCCUPANCY);
ZigbeeTempSensor        zbEnvironment(ZIGBEE_EP_ENVIRONMENT);
ZigbeeIlluminanceSensor zbIlluminance(ZIGBEE_EP_ILLUMINANCE);
ZigbeeAnalog            zbDistance(ZIGBEE_EP_DISTANCE);

bool occupancyState        = false;
bool lastConnectedState    = false;
unsigned long lastReportTime      = 0;
unsigned long buttonPressTime     = 0;
unsigned long buttonLastChangeTime = 0;
bool buttonWasLow          = false;
bool factoryResetTriggered = false;

// Conversione logaritmica Lux ZCL (Cluster 0x0400)
uint16_t luxToZclMeasuredValue(float lux) {
  if (!isfinite(lux) || lux <= 0.0001f) return 0;
  if (lux > 100000.0f) lux = 100000.0f;
  float zcl = 10000.0f * log10f(lux) + 1.0f;
  if (zcl < 1.0f) zcl = 1.0f;
  if (zcl > 0xFFFE) zcl = 0xFFFE;
  return static_cast<uint16_t>(lroundf(zcl));
}

void inviaMisureSensori() {
  float temp = 0.0f;
  float hum  = 0.0f;
  float lux  = 0.0f;
  RadarData radar;

  // 1. SHT40
  if (sensors.getTemperature(temp) && sensors.getHumidity(hum)) {
    zbEnvironment.setTemperature(temp);
    zbEnvironment.setHumidity(hum);
    if (Zigbee.connected()) {
      zbEnvironment.report();
    }
    Serial.printf("[ZIGBEE] SHT40 -> Temp: %.2f °C | Hum: %.2f %%\n", temp, hum);
  }

  // 2. BH1750
  if (sensors.getIlluminance(lux)) {
    uint16_t zclLux = luxToZclMeasuredValue(lux);
    zbIlluminance.setIlluminance(zclLux);
    if (Zigbee.connected()) {
      zbIlluminance.report();
    }
    Serial.printf("[ZIGBEE] BH1750 -> Luce: %.2f lx (ZCL: %u)\n", lux, zclLux);
  }

  // 3. Radar mmWave UART Distanza (Cluster 0x000C)
  if (sensors.getRadarData(radar)) {
    float distCm = static_cast<float>(radar.dist_cm);
    zbDistance.setAnalogInput(distCm);
    if (Zigbee.connected()) {
      zbDistance.reportAnalogInput();
    }
    Serial.printf("[ZIGBEE] LD2410C -> %s | Dist: %.0f cm (Mov: %u cm, En: %u | Stat: %u cm, En: %u)\n",
                  sensors.getRadarStateString(), distCm,
                  radar.moving_dist, radar.moving_energy,
                  radar.stat_dist, radar.stat_energy);
  }
}

void aggiornaPresenza(bool forzaReport = false) {
  bool currentOccupancy = false;
  sensors.getOccupancy(currentOccupancy);
  const bool cambiata = (currentOccupancy != occupancyState);

  if (cambiata) {
    occupancyState = currentOccupancy;
    Serial.printf("[RADAR] Presenza -> %s\n", occupancyState ? "RILEVATA" : "ASSENTE");
  }

  if (cambiata || forzaReport) {
    zbOccupancy.setOccupancy(occupancyState);
    if (Zigbee.connected()) {
      zbOccupancy.report();
    }
  }
}

void gestisciPulsante(unsigned long now) {
  const bool buttonLow = (digitalRead(BUTTON_BOOT_PIN) == LOW);

  if (buttonLow && !buttonWasLow) {
    buttonPressTime        = now;
    buttonLastChangeTime   = now;
    buttonWasLow           = true;
    factoryResetTriggered  = false;
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
    Zigbee.factoryReset();
    delay(1000);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=======================================================");
  Serial.println(" SmartSense Lab - SmartSense Room Zigbee v1.0");
  Serial.println("=======================================================");

  pinMode(BUTTON_BOOT_PIN, INPUT_PULLUP);

  if (sensors.begin()) {
    Serial.println("[HAL] Sensori I2C & Radar inizializzati.");
  } else {
    Serial.println("[HAL] Errore inizializzazione sensori I2C.");
  }

  zbEnvironment.addHumiditySensor(0.0f, 100.0f, 0.1f);
  zbIlluminance.setMinMaxValue(0, 0xFFFE);

  zbDistance.addAnalogInput();
  zbDistance.setAnalogInputDescription("LD2410C distance (cm)");
  zbDistance.setAnalogInputResolution(1.0f);
  zbDistance.setAnalogInputMinMax(0.0f, 600.0f);

  zbOccupancy.setManufacturerAndModel("SmartSense Lab", "SmartSense Room");
  zbEnvironment.setManufacturerAndModel("SmartSense Lab", "SmartSense Room");
  zbIlluminance.setManufacturerAndModel("SmartSense Lab", "SmartSense Room");
  zbDistance.setManufacturerAndModel("SmartSense Lab", "SmartSense Room");

  zbOccupancy.addOTAClient(
      SMARTSENSE_CURRENT_VERSION,
      SMARTSENSE_CURRENT_VERSION,
      SMARTSENSE_HW_VERSION,
      SMARTSENSE_MANUFACTURER_CODE,
      SMARTSENSE_IMAGE_TYPE
  );

  Zigbee.addEndpoint(&zbOccupancy);
  Zigbee.addEndpoint(&zbEnvironment);
  Zigbee.addEndpoint(&zbIlluminance);
  Zigbee.addEndpoint(&zbDistance);

  Serial.println("[ZIGBEE] Avvio router...");
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("[ERRORE] Inizializzazione Zigbee fallita!");
    delay(2000);
    ESP.restart();
  }

  lastReportTime = millis();
}

void loop() {
  const unsigned long now = millis();

  sensors.update();
  gestisciPulsante(now);

  const bool isConnected = Zigbee.connected();
  if (isConnected != lastConnectedState) {
    lastConnectedState = isConnected;
    if (isConnected) {
      Serial.println("\n>>> [ZIGBEE] CONNESSO ALLA RETE MESH <<<");
      aggiornaPresenza(true);
      inviaMisureSensori();
      lastReportTime = millis();
    }
  }

  aggiornaPresenza(false);

  if (now - lastReportTime >= SENSOR_REPORT_INTERVAL_MS) {
    lastReportTime = now;
    if (isConnected) {
      inviaMisureSensori();
    }
  }

  delay(10);
}