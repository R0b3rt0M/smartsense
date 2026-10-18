🏠 SmartSense Lab
Open-Source Edge AI & Smart Home Sensors Ecosystem

SmartSense is an experimental hardware and firmware ecosystem dedicated to building advanced, privacy-focused, and highly reliable multi-sensors for smart home automation (primarily tailored for OpenHAB and Home Assistant via MQTT).

The project focuses on migrating from simple passive components to intelligent Edge AI nodes, evaluating micro-movements, spatial tracking (mmWave), and environmental data, using the latest Espressif architectures (ESP32-C6) and modern IoT protocols.

🎯 Project Goals
Fase 1 - Domestic Prototypes: Build robust, mains-powered, wall-mounted multi-sensors integrating Temperature, Humidity, Illuminance, and Human Presence/Tracking (via 24GHz mmWave radar).

Fase 2 - Connectivity & Reliability: Test and validate modern protocols (Thread, Matter, MQTT, Zigbee), OTA updates, and Edge AI payload processing.

Fase 3 - Product Engineering: Evaluate BOM costs, custom PCB design, SMD manufacturing, and market viability.

📦 Available Firmware Nodes
LivingRoom_ESP32C6 (v0.1 - Current): A complete multi-sensor firmware running on Seeed Studio XIAO ESP32-C6. It reads environmental data via I2C (Adafruit SHT40 + BH1750) and parses a continuous raw hex UART stream from a 24GHz mmWave radar (HLK-LD2410C), packaging everything into a clean JSON payload published over MQTT.

🛠️ Tech Stack
Hardware: Seeed XIAO ESP32-C6, HLK-LD2410C / LD2450 (mmWave), SHT40, BH1750.

Firmware: Arduino Core for ESP32 (v3.x), C++, PubSubClient.

Integration: OpenHAB (via Generic MQTT Thing & JSONPath).