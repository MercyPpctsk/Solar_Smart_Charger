# ESP32-S3-ETH · เครื่องชาร์จไฟโซลาร์เซลล์ + ติดตามคุณภาพอากาศ

เฟิร์มแวร์สำหรับบอร์ด **Waveshare ESP32-S3-ETH** ทำหน้าที่เป็น "ชาร์จเจอร์ + มอนิเตอร์" สำหรับแบตเตอรี่ LiFePO4 ที่ชาร์จด้วยแผงโซลาร์เซลล์ พร้อมดึงข้อมูลคุณภาพอากาศจาก IQAir (สถานีสาธารณะ) และเก็บข้อมูลลง LittleFS/CSV ทุก 1 นาที

> สถานะปัจจุบัน: **`0.4.0-pre-mqtt`** — ทุกค่าที่ TOR กำหนดถูกคำนวณและแคชใน `TelemetryValues` พร้อมให้ส่ง MQTT แล้ว **ยังไม่มี MQTT client** (ส่งต่อให้ขั้นตอนถัดไปผ่านฟังก์ชัน `getValue()`)

---

## สารบัญ
1. [ภาพรวมระบบ](#1-ภาพรวมระบบ)
2. [ฮาร์ดแวร์และการต่อสาย](#2-ฮาร์ดแวร์และการต่อสาย)
3. [สถาปัตยกรรม Core (สำคัญ)](#3-สถาปัตยกรรม-core-สำคัญ)
4. [Build และ Flash](#4-build-และ-flash)
5. [คำสั่ง Serial](#5-คำสั่ง-serial)
6. [Data Flow / วงจรข้อมูล](#6-data-flow--วงจรข้อมูล)
7. [ค่าที่คำนวณแล้ว (TOR 3.2)](#7-ค่าที่คำนวณแล้ว-tor-32)
8. [ส่วน MQTT Dev Handoff](#8-ส่วน-mqtt-dev-handoff-สำหรับคนต่อยอด)
9. [Placeholder / To-do ก่อนลงสนาม](#9-placeholder--to-do-ก่อนลงสนาม)
10. [โครงสร้างโปรเจกต์](#10-โครงสร้างโปรเจกต์)

---

## 1. ภาพรวมระบบ

```
┌─────────────┐    ┌──────────────────────────────┐    ┌──────────┐
│ แผงโซลาร์    │───▶│ ESP32-S3-ETH (ชาร์จเจอร์+มอนิเตอร์)│───▶│ ThingsBoard│
│ (A0/A2)     │    │  • ADS1115 อ่าน V/I 4 ช่อง      │    │ (MQTT)    │
└─────────────┘    │  • SoC coulomb counting        │    └──────────┘
┌─────────────┐    │  • SoH (Equivalent Full Cycles)│         ▲
│ แบต LiFePO4 │───▶│  • IQAir fetch (core 0)        │─────────┘
│ (A1/A3)     │    │  • CSV logging (LittleFS)      │
└─────────────┘    └──────────────────────────────┘
```

**หน้าที่หลัก**
- อ่านแรงดัน/กระแส 4 ช่องผ่าน **ADS1115** (I²C): แผงโซลาร์ (A0/A2) + แบตเตอรี่ (A1/A3)
- คำนวณ **SoC** (State of Charge) ด้วย **coulomb counting** + สอบเทียบที่ endpoint 3.65V/2.50V (LiFePO4)
- คำนวณ **SoH** (State of Health) ด้วย **Equivalent Full Cycles** (สะสม |Ah| → แมปลงอายุการใช้งาน)
- สะสม **energy_in / energy_out** (Wh) รีเซ็ตเที่ยงคืน
- ดึง **คุณภาพอากาศ IQAir** (AQI, PM1, PM2.5, PM10, อุณหภูมิ, ความชื้น, ความดัน) ทุก 30 นาที แบบ non-blocking
- แคชค่าทั้งหมดใน `TelemetryValues` snapshot + mutex ให้ขั้นตอน MQTT อ่านผ่าน `getValue()`

**ข้อจำกัดที่ออกแบบไว้ (hard constraints)**
- ตัวอย่าง ADC **ต้องไม่บล็อก** โดย WiFi/HTTP → ใช้ state machine + แยก core
- SoC ใช้ coulomb counting + endpoint recal เท่านั้น (LiFePO4 แบบ flat curve ใช้ voltage map ไม่ได้)
- ไม่ใช้ `delay()` ในเส้นทางวัด → debounce ปุ่มใช้ millis()

---

## 2. ฮาร์ดแวร์และการต่อสาย

| บอร์ด / ชิ้น | หน้าที่ | หมายเหตุ |
|---|---|---|
| Waveshare ESP32-S3-ETH | MCU หลัก + Ethernet (WiFi 2.4GHz) | 16MB Flash, native USB-Serial/JTAG |
| ADS1115 (16-bit ADC) | วัด V/I 4 ช่อง (A0–A3) | ต่อ I²C |
| ACS712 ×2 | เซ็นเซอร์กระแส (A2=ชาร์จเข้าแบต, A3=โหลด) | สัญญาณ analog เข้า ADS1115 |
| แผงโซลาร์เซลล์ | แหล่งพลังงานชาร์จ | ผ่าน voltage divider เข้า A0 |
| แบตเตอรี่ LiFePO4 | เก็บพลังงาน | ผ่าน voltage divider เข้า A1 |

### Voltage divider (ในโค้ด `voltGain[]`)
- **A0 (โซลาร์): ÷11** (100k/10k) — วัดแรงดันแผงสูง
- **A1 (แบต): ÷2** — วัดแรงดันแบต

> ⚠️ **PCB rev ใหม่:** ช่อง **A1/A2 สลับกัน**จากเลย์เอาต์เดิม — กระแสชาร์จ (ACS712) อยู่ที่ **A2** ส่วน divider แรงดันแบตอยู่ที่ **A1** (ดู `CUR_CH[]`/`VOLT_CH[]`/`voltGain[]` ใน `src/main.cpp`)

> ⚠️ **สำคัญ:** ตอนนี้ทดสอบอยู่บน **1S Li-ion bench cell (~3.3V)** ไม่ใช่ pack 4S จริง ดู `BENCH_1S` ในหัวข้อ [Placeholder](#9-placeholder--to-do-ก่อนลงสนาม)

### LED สถานะ
- WS2812 RGB บน **GPIO21** บอกสถานะผ่าน `neopixelWrite()` (built-in core)

---

## 3. สถาปัตยกรรม Core (สำคัญ)

ESP32-S3 มี 2 core — การแบ่ง core เป็นจุดสำคัญที่สุดของโปรเจกต์นี้ เพื่อให้ ADC sampler **ไม่ถูกบล็อก** โดยงาน WiFi/HTTP ที่อาจค้าง:

| Core | งาน | เหตุผล |
|---|---|---|
| **Core 1** | `loopTask` / `loop()` — สุ่ม ADS1115, SoC, finishReport(), เขียน CSV | งานวัดต้องต่อเนื่องไม่สะดุด |
| **Core 0** | WiFi stack, `iqairTask()` (HTTP fetch 30 นาที), (อนาคต) MQTT task | HTTP/TLS อาจ block หลายวินาที ต้องไม่กระทบ core 1 |

**การสื่อสารระหว่าง core** ใช้ **FreeRTOS mutex** 2 ตัว (ไม่ nest เด็ดขาด — กัน deadlock):
- `iqairMutex` — ป้องกัน `iqairData` (เขียนโดย iqairTask core 0, อ่านโดย loop core 1)
- `telemetryMutex` — ป้องกัน `telemetry` snapshot (เขียนโดย finishReport core 1, อ่านโดย MQTT core 0)

ลำดับการล็อกใน `finishReport()`: เรียก `iqairSnapshot()` (ล็อก/ปล่อย iqairMutex จบก่อน) → ค่อยล็อก `telemetryMutex` → เขียน struct → ปล่อย. **ไม่มีจังหวะที่ล็อกซ้อนกัน**

---

## 4. Build และ Flash

### สิ่งที่ต้องมี
- [PlatformIO Core](https://platformio.org/) (หรือ VS Code + PlatformIO extension)
- บอร์ด Waveshare ESP32-S3-ETH เสียบ USB (native USB-Serial/JTAG)

### การตั้งค่า `config.h`
ไฟล์ `src/config.h` **ถูก `.gitignore` ไว้** (มี WiFi credentials) ต้องสร้างเอง:
```c
#define ENABLE_NETWORK 1        // 1 = WiFi+NTP+CSV+IQAir, 0 = ADC-only
#define BENCH_1S       1        // 1 = bench 1S cell, 0 = pack 4S จริง (ดูหัวข้อ 9)
#define WIFI_SSID      "..."
#define WIFI_PASSWORD  "..."
#define IQAIR_STATION_URL "https://device.iqair.com/v2/<station-id>"
```
> 💡 **มี template พร้อมใช้** — copy แล้วแก้ค่าจริงได้เลย ไม่ต้องเขียนใหม่:
> ```bash
> copy src\config.example.h src\config.h          # แล้วแก้ WIFI_SSID / WIFI_PASSWORD / IQAIR_STATION_URL
> copy tools\iqair\config.example.py tools\iqair\config.py
> ```

### Build + Flash + Monitor
```bash
pio run -t upload          # build + flash ผ่าน COM21
pio device monitor         # เปิด serial monitor 115200
# หรือรวม
pio run -t upload && pio device monitor
```

### ขนาด firmware (หลัง Stage 7)
| | ใช้ไป | จากทั้งหมด | % |
|---|---|---|---|
| **RAM** | 48,600 B | 327,680 B | 14.8% |
| **Flash** | 961,141 B | 6,553,600 B | 14.7% |

---

## 5. คำสั่ง Serial

เปิด serial monitor (115200) แล้วพิมพ์อักขระต่อไปนี้:

| คำสั่ง | หน้าที่ |
|---|---|
| `t` | แสดงสถานะรวม: NTP/WiFi/IQAir/SoC + **telemetry snapshot** (v_solar, v_batt, i_solar, p_solar, i_load, p_load, SoC, SoH, energy, status, alert, uptime) |
| `d` | ดู 20 บรรทัดสุดท้ายของ log CSV |
| `E` | ลบไฟล์ log ทั้งหมดใน LittleFS |
| `i` | บังคับดึง IQAir ทันที (async, รันบน core 0 ไม่บล็อก) |
| `S<%>` | ตั้ง SoC ด้วยมือ เช่น `S90` → 90% (เซฟ NVS) |
| `C<ค่า>` | field-calibrate sensitivity ของ ACS712 |
| `c` | ล้าง field calibration |
| กดปุ่ม **BOOT** | ทำ auto-zero ของ ACS712 (ต้องไม่มีกระแสผ่าน A1/A3 ขณะกด) |
---

## 6. Data Flow / วงจรข้อมูล

```
ทุก ~5.6 วินาที (core 1, loopTask):
  1. สุ่ม ADS1115 ช่อง A0–A3 (state machine, ไม่บล็อก)
  2. คำนวณ V/I จาก mean + calibration (voltGain, vZero, vPerA)
  3. serviceSoC(vBatt, netA)  → coulomb counting + endpoint recal
  4. serviceEnergy(pSolar, pLoad) → สะสม Wh + รีเซ็ตเที่ยงคืน
  5. serviceSoH(chgA, disA)   → สะสม |Ah| + คำนวณ SoH (EFC)
  6. iqairSnapshot(iq)        → อ่าน snapshot IQAir (ล็อก iqairMutex, ปล่อยก่อนขั้นถัดไป)
  7. deriveBatStatus() + deriveSolStatus() + deriveIqStatus() → สถานะ 3 ระบบ
  8. xSemaphoreTake(telemetryMutex) → เขียนทุกฟิลด์ลง `telemetry` → ปล่อย

ทุก 30 นาที (core 0, iqairTask):
  - HTTP GET IQAir → parse JSON (filter เอาแค่ "current") → ล็อก iqairMutex → เขียน iqairData

ทุก 1 นาที (core 1, เมื่อ NTP sync):
  - เขียน 1 บรรทัด CSV ลง LittleFS (v_solar, v_batt, i_charge, i_discharge, timestamp)

NVS persist (กันไฟดับ):
  - SoC: ทุก 5 นาที (KEY_SOC)
  - SoH throughput: ทุก 1 ชั่วโมง (KEY_AH_TP = "ahTp")
  - Auto-zero / field-cal: ทันทีเมื่อสั่ง

อนาคต (core 0, MQTT task):
  - getValue() อ่าน `telemetry` snapshot (bounded 10ms timeout) → publish ขึ้น ThingsBoard
```

### การป้องกัน deadlock / blocking (สำคัญ)
- **ไม่มี `delay()`** ในเส้นทางวัด/telemetry (ตรวจสอบแล้ว — มีแค่ comment)
- **NVS write อยู่นอก mutex**: `prefs.putDouble(KEY_AH_TP,...)` ใน `serviceSoH()` เกิดก่อน `xSemaphoreTake(telemetryMutex)`
- **Mutex ไม่ nest**: `iqairSnapshot()` ปล่อย iqairMutex ก่อน finishReport ล็อก telemetryMutex
- **Reader ใช้ bounded timeout 10ms**: `getValue()` ไม่มีทางบล็อก WiFi/MQTT stack แม้ producer ค้าง

---

## 7. ค่าที่คำนวณแล้ว (TOR 3.2)

ทุกค่าถูกคำนวณใน `finishReport()` และแคชใน `struct TelemetryValues`:

| TOR | ฟิลด์ใน struct | ที่มา / สูตร |
|---|---|---|
| **3.2.2 Voltage Monitor** | `v_solar`, `v_batt` | ADS1115 A0 (÷11), A1 (÷2) |
| **3.2.4 Battery Condition** | `i_solar` (charge in), `p_solar` | A2 → `vSolar × chgA` |
| | `i_load` (discharge out), `p_load` | A3 → `vBatt × disA` |
| | `soc` | coulomb counting + endpoint recal 3.65V/2.50V |
| **3.2.5 Battery Health** | `soh` | Equivalent Full Cycles: `EFC = ahThroughput/(2×CAP)` → `SoH = 100 − (EFC/CYCLE_LIFE)×(100−EOL)` |
| **3.2.6 Energy stats** | `energy_in_wh`, `energy_out_wh` | สะสม `Σ p×dt` (Wh), รีเซ็ตเที่ยงคืน |
| **3.2.1 Notifications** | `bat_status`, `sol_status`, `iq_status` | `deriveBatStatus()` / `deriveSolStatus()` / `deriveIqStatus()` (ดูด้านล่าง) |
| **3.2.6 System** | `uptime_s` | `millis()/1000` |
| **Boot attributes** | `fw_version`, `device_id`, `batt_capacity` | const (ส่งครั้งเดียวเป็น shared attributes) |
| **3.2.6 IQAir** | `aqi_us`, `pm1`, `pm25`, `pm10`, `temp_c`, `humidity`, `pressure`, `iq_ts` | IQAir fetch (core 0) → snapshot |

### `bat_status` (enum string — ระบบแบตเตอรี่, priority สูง→ต่ำ)
`fault` (vBatt ≤ cutoff) | `low_battery` (soc < 20%) | `full` (soc ≥ 99.5% && กำลังชาร์จ) | `charging` | `discharging` | `idle`

### `sol_status` (enum string — ระบบโซลาร์)
`true` (ปกติ) | `fault` (vSolar > Voc max หรือ มีแสง vSolar > 1V แต่ไม่ผลิตกระแส และแบตยังไม่เต็ม)

### `iq_status` (enum string — ระบบ IQAir)
`enable` (WiFi ต่อ + มีข้อมูลล่าสุด) | `fetch_fail` (WiFi ต่อ แต่ดึงข้อมูลไม่ได้) | `offline` (WiFi หลุด)

> 🔧 **`BENCH_1S` guard:** ตอน `BENCH_1S=1` สาขา `fault` ของแบตและ `fault` ของโซลาร์ถูกข้าม (threshold เป็น placeholder สำหรับ pack 12V) เหลือเฉพาะ `low_battery` ที่ทำงาน; `sol_status` ออกมาเป็น `true` ตลอด
---

## 8. ส่วน MQTT Dev Handoff (สำหรับคนต่อยอด)

ขั้นตอนนี้ทิ้งท้ายไว้ให้คนทำ MQTT หยิบค่าไปส่งได้เลย **โดยไม่ต้องแตะ logic วัด/SoC/IQAir เด็ดขาด**

### สัญญา `getValue()` (Contract)
```cpp
// ประกาศใน main.cpp (global)
struct TelemetryValues {
  float v_solar, v_batt, i_solar, p_solar, i_load, p_load;
  float soc, soh, energy_in_wh, energy_out_wh;
  const char *bat_status = "idle";   // full|charging|discharging|low_battery|fault|idle
  const char *sol_status = "true";   // true|fault
  const char *iq_status  = "offline";// enable|fetch_fail|offline
  uint32_t    uptime_s;
  const char *fw_version, *device_id;  float batt_capacity;
  bool iq_valid; int aqi_us; float pm1, pm25, pm10, temp_c, humidity, pressure; String iq_ts;
};
static TelemetryValues getValue();   // คืน copy ส่วนตัว, bounded 10ms, ไม่บล็อก
```

**วิธีใช้ใน MQTT task (core 0):**
```cpp
TelemetryValues tv = getValue();
if (tv.uptime_s == 0) return;   // snapshot ยังไม่พร้อม (setup ยัไม่จบหรือ lock timeout) -> ข้ามรอบ
// ส่ง tv.* ขึ้น ThingsBoard ได้เลย — copy นี้เป็นของ caller ใช้ได้นอก lock
```

### แมปฟิลด์ → ThingsBoard telemetry key (TOR 3.2)
| ฟิลด์ struct | ThingsBoard key | หมวด TOR |
|---|---|---|
| `v_solar` | `v_solar` | 3.2.2 |
| `v_batt` | `v_batt` | 3.2.2 |
| `i_solar` | `i_solar` (charge in) | 3.2.4 |
| `p_solar` | `p_solar` | 3.2.4 |
| `i_load` | `i_load` (discharge out) | 3.2.4 |
| `p_load` | `p_load` | 3.2.4 |
| `soc` | `soc` | 3.2.4 |
| `soh` | `soh` | 3.2.5 |
| `energy_in_wh` | `energy_in` | 3.2.6 |
| `energy_out_wh` | `energy_out` | 3.2.6 |
| `bat_status` | `bat_status` | 3.2.1 |
| `sol_status` | `sol_status` | 3.2.1 |
| `iq_status` | `iq_status` | 3.2.1 |
| `uptime_s` | `uptime` | 3.2.6 |
| `aqi_us`,`pm1`,`pm25`,`pm10` | `aqi`,`pm1`,`pm25`,`pm10` | 3.2.6 |
| `temp_c`,`humidity`,`pressure` | `temp_air`,`humidity`,`pressure` | 3.2.6 |
| `fw_version`,`device_id`,`batt_capacity` | (shared attributes, ส่งครั้งเดียว) | 3.2.6 |

### กฎที่ต้องถือ
1. **ห้ามล็อก mutex ซ้อน** — `getValue()` ล็อก telemetryMutex เท่านั้น, อย่าเรียก iqairSnapshot() ข้างใน
2. **NTP sync ต้องเกิดก่อนรีเซ็ต energy** — รอ `ntpSynced==true` ก่อนอ้างอิง `tm_yday`
3. **Cadence แนะนำ**: ส่ง telemetry ทุก ~10–30 วินาที (finishReport อัปเดตทุก ~5.6 วินาที)
4. **`TB_MQTT_TOKEN`** มีใน `config.h` อยู่แล้ว (placeholder `"your_tb_token"`)

---

## 9. Placeholder / To-do ก่อนลงสนาม

ค่าเหล่านี้เป็น **PLACEHOLDER** — ใช้ทดสอบ logic ได้ แต่ **ห้าม deploy สนามจริง** จนกว่าจะยืนยันตัวเลขจริง:

| ค่า | ค่าปัจจุบัน | ต้องยืนยันจาก |
|---|---|---|
| `BENCH_1S` | `1` (bench 1S) | ตั้งเป็น `0` เมื่อต่อ pack 4S จริง |
| `SOC_HI_V` / `SOC_LO_V` | `3.65` / `2.50` (1S) | เปลี่ยนเป็น 4S endpoint (เช่น 14.6V / 10.0V) |
| `ALERT_VBATT_FAULT_V` | `10.5` | BMS low-voltage cutoff ของ pack จริง |
| `ALERT_VSOLAR_OV_V` | `24.0` | Voc max ของแผงโซลาร์เซลล์จริง |
| `CYCLE_LIFE` | `2000` | spec sheet ของเซลล์ LiFePO4 จริง (premium 4000–6000) |
| `END_OF_LIFE_SOH` | `80` | ธรรมเนียม LiFePO4 (80% capacity) |
| `FW_VERSION` | `"0.4.0-pre-mqtt"` | bump เมื่อ release |
| `DEVICE_ID` | `"solar-01"` | ตั้ง 01–04 ตามหน่วย |
| `TB_MQTT_TOKEN` | `"your_tb_token"` | token จริงจาก ThingsBoard |

**ยังไม่ได้ทำ (เปิดไว้สำหรับขั้นตอนถัดไป):**
- ❌ MQTT client + publish loop (ดูหัวข้อ 8)
- ❌ Light sleep (ประหยัดพลังงานระหว่างรอบวัด)
---

## 10. โครงสร้างโปรเจกต์

```
ESP32-S3-ETH/
├── platformio.ini          # config PlatformIO (ESP32-S3, 16MB flash, COM21)
├── .gitignore              # กัน .pio/ .vscode/ และ src/config.h (มี credentials)
├── README.md               # ไฟล์นี้
├── src/
│   ├── main.cpp            # เฟิร์มแวร์หลัก (sampler, SoC, SoH, telemetry, serial)
│   ├── config.h            # ← ไม่อยู่ใน git (สร้างเอง) — WiFi/IQAir/BENCH_1S
│   └── iqair_client.h      # C++ client ดึง IQAir (7 ฟิลด์, ArduinoJson filter)
├── tools/
│   ├── monitor.py          # serial monitor helper
│   ├── cmd_test.py         # ทดสอบคำสั่ง serial
│   ├── banner.py
│   ├── serial_logger.py    # บันทึก serial ลง serial_log.txt พร้อม timestamp (ไม่รีเซ็ตบอร์ด)
│   └── iqair/
│       ├── config.py
│       └── iqair_client.py # Python reference ของ IQAir client
├── docs/
│   ├── check               # บันทึก audit + checklist TOR 3.2
│   └── wlan_scan.txt
└── backup/                 # snapshot ก่อนแต่ละ stage (ไม่ลง git ก็ได้)
```

### การพึ่งพา (lib_deps ใน `platformio.ini`)
- **ArduinoJson @ ^7.0.0** — parse JSON ของ IQAir (JsonDocument elastic API)
- ส่วน WiFi/HTTPClient/WiFiClientSecure/Preferences/LittleFS/Wire มากับ ESP32 Arduino core

---

## License / หมายเหตุ
- โปรเจกต์ภายใน — อ้างอิง TOR (Terms of Reference) เวอร์ชันที่บันทึกใน `docs/check`
- ค่าทั้งหมดยังไม่ขึ้น ThingsBoard จนกว่าจะเพิ่ม MQTT client (ดูหัวข้อ 8)