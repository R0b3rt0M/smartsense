\# SmartSense Room — Multisensore Zigbee da Salotto



| Proprietà | Valore |

| :--- | :--- |

| \*\*Identificatore Dispositivo\*\* | `MultiSensor-ZR` |

| \*\*Vendor\*\* | SmartSense Lab |

| \*\*Ruolo di Rete Zigbee\*\* | Zigbee 3.0 Router (IEEE 802.15.4, alimentato da rete 5V) |

| \*\*Microcontrollore\*\* | Seeed Studio XIAO ESP32-C6 (RISC-V @ 160 MHz) |

| \*\*Sensori a Bordo\*\* | Sensirion SHT40, ROHM BH1750 (GY-302), Hi-Link HLK-LD2410C |

| \*\*Firmware Target\*\* | v0.4.2 (I2C Recovery, ZCL Raw Lux, UART Radar, OTA Client) |

| \*\*Asset Grafico\*\* | `assets/MultiSensor-ZR.png` |



\---



\## 1. Mappatura Endpoint e Cluster ZCL



Il dispositivo espone 4 endpoint logici conformi alle specifiche Zigbee Cluster Library (ZCL), garantendo compatibilità immediata con Zigbee2MQTT e Home Assistant / OpenHAB:



| Endpoint | Nome Funzione | Cluster ZCL ID | Nome Cluster | Descrizione / Payload |

| :---: | :--- | :---: | :--- | :--- |

| \*\*10\*\* | Occupancy \& OTA | `0x0406`<br>`0x0019` | \*Occupancy Sensing\*<br>\*OTA Upgrade Client\* | Trigger presenza immediato da pin OUT<br>Canale di ricezione aggiornamenti radio |

| \*\*11\*\* | Environment | `0x0402`<br>`0x0405` | \*Temperature Measurement\*<br>\*Relative Humidity Measurement\* | Temperatura ambiente (°C)<br>Umidità relativa (% RH) |

| \*\*12\*\* | Illuminance | `0x0400` | \*Illuminance Measurement\* | Illuminamento espresso in Lux (0–65535 lx) |

| \*\*14\*\* | Radar Distance | `0x000C` | \*Analog Input (Basic)\* | Distanza del target rilevato dal radar (cm) |



\---



\## 2. Proprietà Esposte su Zigbee2MQTT (Payload JSON)



Tutti i dati vengono aggregati e pubblicati automaticamente sul topic:

`zigbee2mqtt/<friendly\_name>`



```json

{

&#x20; "occupancy": true,

&#x20; "distance": 185,

&#x20; "temperature": 23.45,

&#x20; "humidity": 48.20,

&#x20; "illuminance": 12,

&#x20; "linkquality": 84

}

