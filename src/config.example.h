// config.example.h — TEMPLATE.
// Copy this file to src/config.h and fill in YOUR values.
// (src/config.h is git-ignored to keep WiFi credentials out of version control.)
#pragma once

// Build feature toggle. 0 = ADC-only firmware (no WiFi/NTP/CSV logging),
// focused on calibrating the ADS1115 + ACS712 front end. Set to 1 to bring
// back Stage 4 (WiFi+SNTP clock + LittleFS CSV logging).
#define ENABLE_NETWORK 1

// Bench-test guard. 1 = currently on a 1S Li-ion bench cell (~3.3V), NOT the
// real 4S LiFePO4 pack. When 1, deriveStatus()/deriveAlert() SKIP the
// battery-fault and solar-fault branches (their thresholds assume a 12V pack,
// so a 3.3V bench cell would trip "fault" 100% of the time). Only low_battery
// (from SoC) stays active. Set to 0 and confirm SOC_HI_V/SOC_LO_V + the
// ALERT_*_V placeholders before connecting the real 4S pack.
#define BENCH_1S 1

// WiFi credentials (ESP32-S3 supports 2.4GHz only)
#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"

// ThingsBoard MQTT token (unused in this standalone IQAir test)
#define TB_MQTT_TOKEN     "YOUR_TB_TOKEN"

// IQAir public station endpoint (no API key required).
// Use the BASE station endpoint (NO /validated-data suffix): its real-time
// "current" object exposes ALL pollutants pm1 / pm25 / pm10 plus tp/hm/pr,
// matching the IQAir web dashboard. (/validated-data only returns pm25.)
// NOTE: this response is large (~50KB incl. history) but ArduinoJson streams
//       it through a Filter, so only the small "current" object stays in RAM.
#define IQAIR_STATION_URL "https://device.iqair.com/v2/<your-station-id>"