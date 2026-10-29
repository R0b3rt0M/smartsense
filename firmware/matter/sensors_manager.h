#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_SHT4x.h"
#include <BH1750.h>

// ============================================================
// SmartSense Lab - Sensors Hardware Abstraction Layer (HAL)
// Hardware: Seeed XIAO ESP32-C6
// Supporto: SHT40 (I2C), BH1750 (I2C), HLK-LD2410C (UART + OUT)
// ============================================================

struct RadarData {
  bool     uart_ok       = false;
  uint8_t  target_state  = 0; // 0=none, 1=moving, 2=stat, 3=both
  uint16_t dist_cm       = 0;
  uint16_t moving_dist   = 0;
  uint8_t  moving_energy = 0;
  uint16_t stat_dist     = 0;
  uint8_t  stat_energy   = 0;
};

struct SensorHealth {
  bool is_initialized = false;
  bool last_read_ok   = false;
  unsigned long last_valid_timestamp = 0;
};

class SensorsManager {
public:
  SensorsManager(uint8_t sda_pin, uint8_t scl_pin,
                 uint8_t radar_out_pin, uint8_t radar_rx_pin, uint8_t radar_tx_pin);

  bool begin();
  void update(); // Chiamata continua nel loop principale

  // Lettura congiunta ottimizzata I2C (singola transazione per SHT40)
  bool getEnvironmental(float &temp_c, float &hum_pct);

  // Letture con diagnostica di validita
  bool getTemperature(float &temp_c);
  bool getHumidity(float &hum_pct);
  bool getIlluminance(float &lux);
  bool getOccupancy(bool &is_occupied);
  bool getRadarData(RadarData &data);

  // Diagnostica di salute hardware
  SensorHealth getSHT40Health()   const { return _sht40Health; }
  SensorHealth getBH1750Health()  const { return _bh1750Health; }
  SensorHealth getRadarHealth()   const { return _radarHealth; }

  const char* getRadarStateString() const;

private:
  uint8_t _sda_pin;
  uint8_t _scl_pin;
  uint8_t _radar_out_pin;
  uint8_t _radar_rx_pin;
  uint8_t _radar_tx_pin;

  // Istanze sensori e seriale
  Adafruit_SHT4x _sht4x;
  BH1750         _lightMeter;
  HardwareSerial _radarSerial;

  // Cache delle ultime letture valide
  float     _cached_temp = 0.0f;
  float     _cached_hum  = 0.0f;
  float     _cached_lux  = 0.0f;
  bool      _cached_occ  = false;
  RadarData _cached_radar;

  // Tracciamento stato sensori
  SensorHealth _sht40Health;
  SensorHealth _bh1750Health;
  SensorHealth _radarHealth;

  // Buffer circolare / sliding window per UART radar
  uint8_t _rxBuf[64];
  uint8_t _rxIdx = 0;
  unsigned long _lastRadarFrameMs = 0;
  static constexpr unsigned long RADAR_UART_TIMEOUT_MS = 3000UL;

  void _parseRadarUART();
  void _shiftRxBuffer(uint8_t count);
};