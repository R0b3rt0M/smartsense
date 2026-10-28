#include "sensors_manager.h"

SensorsManager::SensorsManager(uint8_t sda_pin, uint8_t scl_pin,
                               uint8_t radar_out_pin, uint8_t radar_rx_pin, uint8_t radar_tx_pin)
  : _sda_pin(sda_pin),
    _scl_pin(scl_pin),
    _radar_out_pin(radar_out_pin),
    _radar_rx_pin(radar_rx_pin),
    _radar_tx_pin(radar_tx_pin),
    _radarSerial(1) {}

bool SensorsManager::begin() {
  pinMode(_radar_out_pin, INPUT);

  // 1. Inizializzazione bus I2C
  Wire.begin(_sda_pin, _scl_pin);

  // 2. Inizializzazione SHT40
  if (_sht4x.begin(&Wire)) {
    _sht4x.setPrecision(SHT4X_HIGH_PRECISION);
    _sht4x.setHeater(SHT4X_NO_HEATER);
    _sht40Health.is_initialized = true;
  } else {
    _sht40Health.is_initialized = false;
  }

  // 3. Inizializzazione BH1750
  if (_lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
    _bh1750Health.is_initialized = true;
  } else {
    _bh1750Health.is_initialized = false;
  }

  // 4. Inizializzazione UART per HLK-LD2410C (256000 baud)
  _radarSerial.begin(256000, SERIAL_8N1, _radar_rx_pin, _radar_tx_pin);
  _radarHealth.is_initialized = true;

  return (_sht40Health.is_initialized && _bh1750Health.is_initialized);
}

void SensorsManager::_shiftRxBuffer(uint8_t count) {
  if (count >= _rxIdx) {
    _rxIdx = 0;
  } else {
    memmove(_rxBuf, _rxBuf + count, _rxIdx - count);
    _rxIdx -= count;
  }
}

void SensorsManager::_parseRadarUART() {
  while (_radarSerial.available()) {
    if (_rxIdx < sizeof(_rxBuf)) {
      _rxBuf[_rxIdx++] = _radarSerial.read();
    } else {
      // Buffer saturo: scarta il byte piu vecchio (sliding window)
      _shiftRxBuffer(1);
      _rxBuf[_rxIdx++] = _radarSerial.read();
    }

    // Ricerca dell'header standard LD2410 (0xF4 0xF3 0xF2 0xF1)
    while (_rxIdx >= 4) {
      if (_rxBuf[0] == 0xF4 && _rxBuf[1] == 0xF3 && _rxBuf[2] == 0xF2 && _rxBuf[3] == 0xF1) {
        break;
      }
      _shiftRxBuffer(1);
    }

    if (_rxIdx >= 6) {
      uint16_t payloadLen = _rxBuf[4] | (_rxBuf[5] << 8);
      uint16_t totalFrameLen = 4 + 2 + payloadLen + 4;

      if (payloadLen < 11 || totalFrameLen > sizeof(_rxBuf)) {
        _shiftRxBuffer(1);
        continue;
      }

      if (_rxIdx >= totalFrameLen) {
        if (_rxBuf[totalFrameLen - 4] == 0xF8 &&
            _rxBuf[totalFrameLen - 3] == 0xF7 &&
            _rxBuf[totalFrameLen - 2] == 0xF6 &&
            _rxBuf[totalFrameLen - 1] == 0xF5) {

          if ((_rxBuf[6] == 0x02 || _rxBuf[6] == 0x01) && _rxBuf[7] == 0xAA) {
            _cached_radar.target_state  = _rxBuf[8] & 0x03;
            _cached_radar.moving_dist   = _rxBuf[9]  | (_rxBuf[10] << 8);
            _cached_radar.moving_energy = _rxBuf[11];
            _cached_radar.stat_dist     = _rxBuf[12] | (_rxBuf[13] << 8);
            _cached_radar.stat_energy   = _rxBuf[14];
            _cached_radar.dist_cm       = _rxBuf[15] | (_rxBuf[16] << 8);

            _lastRadarFrameMs            = millis();
            _cached_radar.uart_ok        = true;
            _radarHealth.last_read_ok    = true;
            _radarHealth.last_valid_timestamp = _lastRadarFrameMs;
          }
        }
        _shiftRxBuffer(totalFrameLen);
      }
    }
  }

  if (_cached_radar.uart_ok && (millis() - _lastRadarFrameMs > RADAR_UART_TIMEOUT_MS)) {
    _cached_radar.uart_ok = false;
    _radarHealth.last_read_ok = false;
  }
}

void SensorsManager::update() {
  _parseRadarUART();
  _cached_occ = (digitalRead(_radar_out_pin) == HIGH);
}

bool SensorsManager::getTemperature(float &temp_c) {
  if (!_sht40Health.is_initialized) return false;

  sensors_event_t h, t;
  if (_sht4x.getEvent(&h, &t)) {
    if (isfinite(t.temperature) && t.temperature > -40.0f && t.temperature < 85.0f) {
      _cached_temp = t.temperature;
      _cached_hum  = h.relative_humidity;
      _sht40Health.last_read_ok = true;
      _sht40Health.last_valid_timestamp = millis();
      temp_c = _cached_temp;
      return true;
    }
  }
  _sht40Health.last_read_ok = false;
  return false;
}

bool SensorsManager::getHumidity(float &hum_pct) {
  if (!_sht40Health.is_initialized) return false;

  sensors_event_t h, t;
  if (_sht4x.getEvent(&h, &t)) {
    if (isfinite(h.relative_humidity) && h.relative_humidity >= 0.0f && h.relative_humidity <= 100.0f) {
      _cached_temp = t.temperature;
      _cached_hum  = h.relative_humidity;
      _sht40Health.last_read_ok = true;
      _sht40Health.last_valid_timestamp = millis();
      hum_pct = _cached_hum;
      return true;
    }
  }
  _sht40Health.last_read_ok = false;
  return false;
}

bool SensorsManager::getIlluminance(float &lux) {
  if (!_bh1750Health.is_initialized) return false;

  const float read_lux = _lightMeter.readLightLevel();
  if (isfinite(read_lux) && read_lux >= 0.0f && read_lux <= 65535.0f) {
    _cached_lux = read_lux;
    _bh1750Health.last_read_ok = true;
    _bh1750Health.last_valid_timestamp = millis();
    lux = _cached_lux;
    return true;
  }
  _bh1750Health.last_read_ok = false;
  return false;
}

bool SensorsManager::getOccupancy(bool &is_occupied) {
  is_occupied = _cached_occ;
  return true;
}

bool SensorsManager::getRadarData(RadarData &data) {
  if (_cached_radar.uart_ok) {
    data = _cached_radar;
    return true;
  }
  return false;
}

const char* SensorsManager::getRadarStateString() const {
  switch (_cached_radar.target_state) {
    case 0:  return "none";
    case 1:  return "moving";
    case 2:  return "stationary";
    case 3:  return "both";
    default: return "unknown";
  }
}