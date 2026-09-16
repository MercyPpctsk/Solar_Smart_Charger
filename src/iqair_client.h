// iqair_client.h
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

struct IQAirData {
  bool  valid      = false;
  int   aqi_us     = 0;
  float pm1        = 0.0f;       // µg/m3
  float pm25       = 0.0f;       // µg/m3
  float pm10       = 0.0f;       // µg/m3
  float temp_c     = 0.0f;       // °C
  float humidity   = 0.0f;       // %RH
  float pressure   = 0.0f;       // mbar (== hPa)
  String ts;                    // เก็บ timestamp จาก IQAir ไว้เทียบ stale
  unsigned long last_fetch_ms   = 0;   // เวลาที่ fetch "สำเร็จ" ล่าสุด
  unsigned long last_attempt_ms = 0;   // เวลาที่ "พยายามยิง" ล่าสุด (กัน retry storm)
};

class IQAirClient {
public:
  explicit IQAirClient(const char* stationUrl) : _url(stationUrl) {}

  // เรียกได้ตรงๆ ไม่ต้อง check interval เอง — เช็คให้แล้วข้างใน
  // คืน true เฉพาะตอนที่ "ยิง request จริง" (ไม่ว่าจะสำเร็จหรือ fail)
  // เพื่อให้ caller รู้ว่ารอบนี้ใช้ WiFi/power ไปหรือเปล่า
  //
  // retryMs = ระยะถี่สุดที่ยอมให้ retry เมื่อยังไม่เคย fetch สำเร็จ (backoff)
  // ป้องกันไม่ให้ยิงรัวทุก loop cycle ตอน fetch แรกๆ ล้มเหลว (เช่น WiFi เพิ่งต่อ)
  bool pollIfDue(IQAirData &out, unsigned long intervalMs,
                 unsigned long retryMs = 15000) {
    unsigned long now = millis();

    bool due;
    if (out.last_fetch_ms == 0) {
      // ยังไม่เคยสำเร็จ: ยิงครั้งแรกได้เลย จากนั้นเว้นอย่างน้อย retryMs ต่อครั้ง
      due = (out.last_attempt_ms == 0) ||
            (now - out.last_attempt_ms >= retryMs);
    } else {
      // เคยสำเร็จแล้ว: ยึดตาม interval ปกติ
      due = (now - out.last_fetch_ms >= intervalMs);
    }
    if (!due) return false;

    out.last_attempt_ms = now;
    fetch(out);
    return true;
  }

  bool fetch(IQAirData &out) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[IQAir] WiFi not connected, skip");
      return false;
    }

    WiFiClientSecure client;
    client.setInsecure();          // endpoint นี้ public ไม่มี auth — แต่ยังควร verify cert ใน production
    client.setTimeout(8000);

    HTTPClient https;
    if (!https.begin(client, _url)) {
      Serial.println("[IQAir] begin() failed");
      return false;
    }

    unsigned long t0 = millis();
    int code = https.GET();
    unsigned long elapsed = millis() - t0;

    if (code != HTTP_CODE_OK) {
      Serial.printf("[IQAir] HTTP error: %d (took %lums)\n", code, elapsed);
      https.end();
      return false;
    }

    // The base station endpoint uses SHORT field names inside "current":
    //   pm1/pm25/pm10 -> { conc, aqius, aqicn }   tp=°C  hm=%RH  pr=Pascals
    //   mainus = dominant pollutant (US AQI is the max of the sub-indices).
    // We keep whole pollutant objects (conc + aqius) so grab them wholesale.
    JsonDocument filter;
    filter["current"]["ts"]     = true;
    filter["current"]["mainus"] = true;
    filter["current"]["pm1"]    = true;   // { conc, aqius, aqicn }
    filter["current"]["pm25"]   = true;
    filter["current"]["pm10"]   = true;
    filter["current"]["tp"]     = true;
    filter["current"]["hm"]     = true;
    filter["current"]["pr"]     = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, https.getStream(), DeserializationOption::Filter(filter));
    https.end();

    if (err) {
      Serial.printf("[IQAir] JSON parse error: %s\n", err.c_str());
      return false;
    }

    JsonObject current = doc["current"];
    if (current.isNull()) {
      Serial.println("[IQAir] unexpected response shape");
      return false;
    }

    String newTs = current["ts"] | "";
    bool stale = (newTs.length() > 0 && newTs == out.ts);

    out.pm1       = current["pm1"]["conc"]  | 0.0f;
    out.pm25      = current["pm25"]["conc"] | 0.0f;
    out.pm10      = current["pm10"]["conc"] | 0.0f;
    out.temp_c    = current["tp"] | 0.0f;
    out.humidity  = current["hm"] | 0.0f;
    out.pressure  = (current["pr"] | 0.0f) / 100.0f;   // Pascals -> mbar (==hPa)

    // US AQI overall = the highest pollutant sub-index (== the "mainus" one).
    int aqiPm25 = current["pm25"]["aqius"] | 0;
    int aqiPm10 = current["pm10"]["aqius"] | 0;
    int aqiPm1  = current["pm1"]["aqius"]  | 0;
    out.aqi_us  = max(aqiPm25, max(aqiPm10, aqiPm1));

    out.ts        = newTs;
    out.valid     = true;
    out.last_fetch_ms = millis();

    Serial.printf("[IQAir] (%lums) ts=%s%s AQI=%d PM1=%.1f PM2.5=%.1f PM10=%.1f "
                  "T=%.1fC RH=%.0f%% P=%.1fmbar\n",
                  elapsed, newTs.c_str(), stale ? " STALE" : "",
                  out.aqi_us, out.pm1, out.pm25, out.pm10,
                  out.temp_c, out.humidity, out.pressure);
    return true;
  }

private:
  const char* _url;
};
