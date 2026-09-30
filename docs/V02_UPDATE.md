# บันทึกการอัปเดตเฟิร์มแวร์ Solar Smart Charger (Version 0.2)

**วันที่บันทึก**: 17 กันยายน 2026  
**อุปกรณ์เป้าหมาย**: Waveshare ESP32-S3-ETH (ESP32-S3-WROOM-1, 16MB Flash)  
**พอร์ตการสื่อสาร**: Native USB-Serial/JTAG (COM11) @ 115200 baud  

---

## 1. ภาพรวมการเปลี่ยนแปลงใน V02 (Summary of V02 Update)

ในเวอร์ชันนี้ เป็นการนำสถาปัตยกรรมระบบ **Web UI Configuration Portal**, **SoftAP ความเสถียรสูง**, และ **ThingsBoard MQTT Telemetry Integration** เข้ามาผสานกับตรรกะใหม่ของ V02 โดยคงตรรกะการคำนวณหลักทั้งหมด (ADC Non-blocking sampling 11ms, Coulomb counting SoC, LittleFS Logging, Zero calibration NVS) ไว้อย่างสมบูรณ์ 100%

---

## 2. ตัวแปรและคีย์ใหม่ใน V02 (New Subsystem Status Keys)

ใน V02 มีการเปลี่ยนโครงสร้างสถานะระบบจากการใช้ `status` และ `alert` รวมเดิม มาเป็นการแยกสถานะเป็น **3 ระบบย่อย (Subsystems)** ในโครงสร้าง `struct TelemetryValues`:

| คีย์ Telemetry | ชนิดข้อมูล | ค่าที่เป็นไปได้ | คำอธิบาย |
|---|---|---|---|
| **`bat_status`** | String | `full` \| `charging` \| `discharging` \| `low_battery` \| `fault` \| `idle` | สถานะของระบบแบตเตอรี่ (คำนวณจาก `deriveBatStatus()`) |
| **`sol_status`** | String | `true` \| `fault` | สถานะของแผงโซลาร์เซลล์ (ตรวจจับ Over-voltage หรือมีแสงแต่ไม่มีไฟชาร์จ `deriveSolStatus()`) |
| **`iq_status`** | String | `enable` \| `fetch_fail` \| `offline` | สถานะการเชื่อมต่อและการดึงข้อมูลสภาพอากาศ IQAir (`deriveIqStatus()`) |

### เกณฑ์การตรวจวัดเพิ่มเติม:
- `SOLAR_DAY_THRESHOLD_V = 1.0f`: แรงดันขั้นต่ำที่ถือว่าแผงโซลาร์ได้รับแสงแดด
- `ALERT_SOC_LOW_PCT = 20.0f`: ระดับแบตเตอรี่ต่ำเข้าสู่สถานะ `low_battery`
- `ALERT_VBATT_FAULT_V = 10.5f`: เกณฑ์ BMS Cutoff สำหรับแบตเตอรี่ 4S (ข้ามเมื่อเปิดโหมด `BENCH_1S`)
- `ALERT_VSOLAR_OV_V = 24.0f`: เกณฑ์แรงดันเกินของแผงโซลาร์ (Over Voc)

---

## 3. รายการ Telemetry & Attributes ที่ส่งขึ้น ThingsBoard

ทุกค่าจะถูกส่งไปยัง MQTT Topic: `v1/devices/me/telemetry` โดยอัตโนมัติ

### 3.1 ข้อมูลชุดโซลาร์และแบตเตอรี่ (ส่งตามรอบความถี่ `tb_interval`)
```json
{
  "v_solar": 19.30,
  "v_batt": 3.45,
  "i_solar": 1.68,
  "p_solar": 32.4,
  "i_load": 0.90,
  "p_load": 3.1,
  "soc": 55.8,
  "soh": 100.0,
  "energy_in": 0.00,
  "energy_out": 0.00,
  "bat_status": "charging",
  "sol_status": "true",
  "iq_status": "offline",
  "uptime": 125
}
```

### 3.2 ข้อมูลสภาพอากาศ IQAir (ส่งแยกต่างหากเมื่อมีข้อมูลสดใหม่)
ดึงข้อมูลจาก IQAir Station ผ่าน HTTPS GET บน Core 0 และยิงขึ้น ThingsBoard ทันทีที่ดึงข้อมูลรอบใหม่สำเร็จ:
```json
{
  "aqi": 45,
  "pm1": 5.2,
  "pm25": 11.4,
  "pm10": 18.0,
  "temp_air": 29.5,
  "humidity": 68,
  "pressure": 1012.3
}
```

### 3.3 Boot Attributes (ส่งครั้งเดียวเมื่อเชื่อมต่อ MQTT สำเร็จ)
ส่งไปยัง Topic: `v1/devices/me/attributes`
```json
{
  "fw_version": "0.4.0-pre-mqtt",
  "device_id": "solar-01",
  "batt_capacity": 150.0
}
```

---

## 4. ระบบ Web UI & SoftAP Portal (`setup_web.h`)

### 4.1 การเชื่อมต่อเริ่มต้น
- **SoftAP SSID**: `SOLAR-SETUP-<MAC>` (เช่น `SOLAR-SETUP-D7DDE8`)
- **Password**: `solar1234`
- **Portal IP**: `192.168.4.1` (มี Captive DNS Port 53 เด้งเข้าหน้าตั้งค่าอัตโนมัติ)
- สามารถเข้าผ่าน IP ขาบ้าน/ออฟฟิศได้เช่นกัน (เช่น `http://192.168.0.128`)

### 4.2 หน้าต่างควบคุม (2 แท็บหลัก ดีไซน์ Dark Cyberpunk `#00d4ff` / `#1a1a2e`):
1. **แท็บ 1: WiFi Settings**:
   - กำหนด WiFi SSID / Password ที่จะให้ตัวบอร์ดเชื่อมต่อ
   - แสดงตารางข้อมูลเครือข่าย: สถานะการเชื่อมต่อ, Station IP, SoftAP IP, RSSI Signal Strength, MAC Address
2. **แท็บ 2: ThingsBoard & IQAir Settings**:
   - **ThingsBoard Host / Port / Access Token / Telemetry Topic**
   - **Telemetry Interval (Seconds)**: ความถี่ในการส่งค่าโซลาร์/แบตเตอรี่ (ตั้งได้ 5 – 3600 วินาที, ดีฟอลต์ 15 วินาที)
   - **IQAir Public Station URL**: ใส่ URL สดของสถานี IQAir พร้อมระบบ Auto-Sanitize ตัดเครื่องหมายคำพูด `"` หรือ `'` ท้ายข้อความทิ้งอัตโนมัติ
   - **IQAir Fetch Interval (Minutes)**: ความถี่ในการดึงข้อมูลจาก IQAir (ตั้งได้ 1 – 1440 นาที, ดีฟอลต์ 30 นาที)
   - **ปุ่ม Restart Device**: สั่งรีบูตตัวประมวลผลผ่านซอฟต์แวร์ทันที

### 4.3 กลไกความเสถียรของ SoftAP (ป้องกันปัญหา AP หาย):
- ปิดระบบประหยัดพลังงานของชิป Wi-Fi: `esp_wifi_set_ps(WIFI_PS_NONE)` และ `WiFi.setSleep(false)`
- ปิด `WiFi.setAutoReconnect(false)` เพื่อป้องกันไม่ให้ Wi-Fi Driver ทำการสแกนค้นหาช่องสัญญาณแบบ Channel Hopping รบกวน Beacon ของ SoftAP
- มีระบบตรวจจับ `WiFi.softAPgetStationNum() > 0` เพื่อชะลอการเชื่อมต่อ Station ในขณะที่ผู้ใช้กำลังเชื่อมต่ออยู่บนหน้าเว็บพอร์ทัล

---

## 5. การแก้ไขทิศทางเซนเซอร์วัดกระแส (Current Sensor Polarity Fix)

จากการทดสอบจริงพบว่า ฮาร์ดแวร์เซนเซอร์ ACS712 ทั้ง 2 ช่องถูกต่อวงจรกลับทิศทางกัน:
- **A1 (Charge เข้าแบต)**: กระแสไหลเข้า ทำให้ $V_{out}$ สูงกว่า $V_{zero}$ ($\Delta V > 0$) $\rightarrow$ ตัวคูณ $+1.0$
- **A3 (Discharge จ่ายโหลด)**: กระแสไหลออก ทำให้ $V_{out}$ ดรอปต่ำกว่า $V_{zero}$ ($\Delta V < 0$) $\rightarrow$ หากใช้ตัวคูณเดิม $+1.0$ จะทำให้ค่ากระแสติดลบ

### การแก้ไข (PCB rev เดิม):
แยกตัวคูณทิศทางในโค้ด:
```cpp
static const float CUR_SIGN_FLIP_CHG =  1.0f;  // A1 rises above zero when charging
static const float CUR_SIGN_FLIP_DIS = -1.0f;  // A3 drops below zero when discharging
```
- ผลลัพธ์: กระแสโหลด `Dis` และ `i_load` แสดงผลเป็น **ค่าบวก (`+0.90 A`)**
- สมการ `Net = chgA - disA` คำนวณหักลบกระแสได้อย่างถูกต้อง ($1.68 - 0.90 = \mathbf{+0.78\text{ A}}$)
- ฟังก์ชัน `deriveBatStatus()` สามารถตรวจจับและเข้าเงื่อนไขสถานะ `"discharging"` ได้อย่างถูกต้อง
### อัปเดตสำหรับ PCB rev ใหม่ (A1/A2 สลับกัน):
บน PCB rev ใหม่ มีการสลับช่อง A1↔A2 และทิศทางเซนเซอร์ Discharge กลับด้านจากเดิม:
- **A2 (Charge เข้าแบต)**: $V_{out}$ ยัง **สูงกว่า** $V_{zero}$ ตอนชาร์จ $\\rightarrow$ ใช้ $+1.0$ (เหมือนเดิม)
- **A3 (Discharge จ่ายโหลด)**: $V_{out}$ **สูงกว่า** $V_{zero}$ ตอนเปิดโหลด (กลับขั้วจากเดิม) $\\rightarrow$ ใช้ $+1.0$ อีกครั้ง

```cpp
static const float CUR_SIGN_FLIP_CHG =  1.0f;  // A2 rises above zero when charging
static const float CUR_SIGN_FLIP_DIS =  1.0f;  // A3 also rises above zero when load draws current (new PCB)
```
- ยืนยันด้วยการทดสอบจริง 2026-09-23: ใช้ `-1.0f` เดิมทำให้ Dis ติดลบเวลาเปิดโหลด เปลี่ยนเป็น `+1.0f` แล้วค่ากลับเป็นบวกตามที่คาดไว้

### Clamp ค่ากระแสติดลบเป็นศูนย์ (2026-09-23)

หลังจากแก้ทิศทางเซนเซอร์แล้ว ยังพบค่าติดลบขนาดเล็กจาก noise รอบจุด zero เช่น `Chg: -0.001 A` หรือ `Dis: -0.029 A` เมื่อไม่มีกระแสจริง และใน log ระยะยาว (1 ชม.) เคยพบ reverse current ขนาด `-0.45 A` ตอนกลางคืน (แบตไหลย้อนกลับเข้าแผงมืด — บอร์ดนี้ไม่มี blocking diode) ซึ่งทำให้ Telemetry และ SoC ผิดเพี้ยน

การแก้ไข: clamp ค่าติดลบเป็นศูนย์ทันทีหลังคำนวณกระแส ก่อนส่งต่อไปยังทุกส่วน:
```cpp
if (chgA < 0.0f) chgA = 0.0f;   // noise รอบ zero / reverse current กลางคืน
if (disA < 0.0f) disA = 0.0f;
```
- ครอบคลุมทุกช่องทาง: `Net`, SoC/Energy/SoH integration, MQTT Telemetry (`i_solar`, `i_load`) และ CSV log ใช้ค่า clamped ชุดเดียวกัน (`logAgg.add()` ถูกเปลี่ยนให้ใช้ `chgA`/`disA` แทนการคำนวณใหม่จาก raw)
- **ไม่กระทบการสอบเทียบ**: ค่า raw volts ใน serial report (`raw A2= ... A3= ...`) และการ re-zero ด้วยปุ่ม BOOT ยังใช้ค่าดิบไม่ถูก clamp
- ผลทดสอบจริง 2026-09-23: ก่อน clamp เห็น `Chg: -0.001 A, Dis: -0.029 A` หลัง clamp เห็น `Chg: +0.000/+0.001 A, Dis: +0.000 A` เสมอ

---

## 6. สถาปัตยกรรมแบ่งคอร์ (Core 0 vs Core 1 Separation)

- **Core 0 (Network & Web)**:
  - `tbMqttTask`: จัดการการเชื่อมต่อ MQTT, ยิง Telemetry ทุกรอบความถี่, และคอยส่งค่า IQAir เมื่อมีข้อมูลสด
  - `iqairTask`: จัดการเชื่อมต่อ HTTPS GET ไปดึงข้อมูลสถานี IQAir
  - `setupWebTick()`: ให้บริการ Web Server และ Captive DNS Server
- **Core 1 (Real-time Sampling & Calculation)**:
  - `runSampler()`: State machine อ่าน ADS1115 รอบละ 11ms (ไม่มีการ Block delay)
  - `serviceSoC()`: คำนวณ Coulomb Counting บันทึก NVS ทุก 5 นาที
  - `serviceSoH()`: คำนวณ Equivalent Full Cycles (EFC)
  - `serviceEnergy()`: คำนวณ Wh เข้า/ออก สะสมและรีเซ็ตทุกเที่ยงคืน

---

## 7. ผลการทดสอบบนอุปกรณ์จริง (Verification Test Log)

```text
=== ESP32-S3-ETH Solar charger: A0=v_solar A1=i_charge A2=v_batt A3=i_discharge ===
ADC 0x48 @ SDA=1/SCL=2 | PGA=+/-6.144V | non-blocking sampler
Using stored NVS zero: A1=2.3086 V, A3=2.3043 V, sens=63.1 mV/A  [FIELD-CAL]
[CFG ] Loaded: SSID='slixrouter' TB='thingsboard.weaverbase.com:1883' (int=120s) Token='9YLXFadM8n7vfP2n8rWv' IQAir='https://device.iqair.com/v2/...' (int=30min)
[SETUP] SoftAP 'SOLAR-SETUP-D7DDE8' -> OK, IP: 192.168.4.1 (Password: solar1234)
[SETUP] Web server listening on port 80
[NET ] WiFi connecting to "slixrouter"; NTP pool.ntp.org,time.google.com (TZ UTC-7)
[LOG ] LittleFS ok, 32/3456 KB used
[SoC ] starting at 55.8% (cap 150 Ah, LiFePO4 3.65V/2.5V endpoints)
[TB  ] task started (core 0), waiting for WiFi...
[NET ] WiFi up, IP 192.168.0.128
[TIME] NTP synced: 2026-09-17 18:42:36
[TB  ] Connected to ThingsBoard!
Solar: 19.297 V  Batt:  3.453 V | Chg:  +1.676 A (pp 0.345)  Dis:  +0.900 A (pp 0.348) | Net:  +0.776 A | SoC:  55.8%
[TB  ] Telemetry sent (228 B) [OK]
[TB  ] Attributes sent (74 B): {"fw_version":"0.4.0-pre-mqtt","device_id":"solar-01","batt_capacity":150} [OK]
```

บันทึก ณ วันที่ 17 กันยายน 2026 โดย AI Assistant

---

## 8. อัปเดต 2026-09-30 — Voltage Offset Calibration + Daily Auto-Restart

### 8.1 Daily auto-restart ที่ 00:01 น.
เพิ่ม `serviceDailyRestart()` เพื่อรีบูตบอร์ดอัตโนมัติทุกวันเวลา 00:01 น. (เวลาท้องถิ่น หลัง NTP sync) เพื่อความเสถียรระยะยาว
- ตรวจจับโดยเก็บ `lastRestartDay` (จาก `tm.tm_yday`) เทียบทุก 30 วินาทีใน `loop()` (core 1)
- ไม่ใช้ `delay()` → ไม่กระทบเส้นทางวัด/telemetry 11ms
- ค่า latch โหลดจาก NVS ตอน `setup()` เพื่อกันรีบูตซ้ำในวันเดียวกันหลังไฟตก/ brown-out
- มี `#else` stub สำหรับ ADC-only build (`ENABLE_NETWORK=0`) เพื่อให้ `setup()`/`loop()` เรียกได้โดยไม่ต้อง `#if` ทุก call site

### 8.2 Voltage offset calibration สำหรับ v_batt และ v_solar
เพิ่ม offset แบบ additive แก้ความคลาดเคลื่อนระหว่างแรงดันที่ ADC อ่านได้กับค่าจริงจากมัลติมิเตอร์ ทั้งสองช่องวัดแรงดัน:

| ช่อง | ADC | Config default | Serial เซ็ต | Serial เคลียร์ | NVS key |
|---|---|---|---|---|---|
| v_batt | A1 | `VBATT_OFFSET_V` | `V<x>` | `v` | `vbOfs` / `vbOfsOk` |
| v_solar | A0 | `VSOLAR_OFFSET_V` | `P<x>` (P=Panel) | `p` | `vsOfs` / `vsOfsOk` |

**รูปแบบเดียวกับ field-cal sensitivity เดิม** (`C`/`c`): มี default ใน `config.h` เป็น safety net, override ผ่าน serial เก็บ NVS ข้าม reboot, range check `±1.0V` กันค่าขยะ

#### Sign convention (สำคัญ)
offset ถูก **บวกเข้า**กับแรงดันที่คำนวณจาก ADC หลังคูณ `voltGain[]`:
- **ADC อ่านต่ำกว่ามัลติมิเตอร์** → ใช้ค่า **บวก** (เช่น `V0.2` / `P0.1`) เพื่อดันขึ้น
- **ADC อ่านสูงกว่ามัลติมิเตอร์** → ใช้ค่า **ลบ** (เช่น `V-0.2` / `P-0.1`) เพื่อลดลง

#### จุด apply (จุดเดียว → กระจายอัตโนมัติ)
ทั้งสอง offset ใส่ที่จุดประกาศตัวแปรใน `finishReport()` บรรทัดเดียว แล้วใช้ตัวแปรนั้นทุกที่:
```cpp
float vSolar = acc[VOLT_CH[0]].mean() * voltGain[VOLT_CH[0]] + vSolarOffsetV;
float vBatt  = acc[VOLT_CH[1]].mean() * voltGain[VOLT_CH[1]] + vBattOffsetV;
```
→ กระจายไป: Serial print, CSV log, `serviceSoC()` (endpoint recal 3.65V/2.50V), `pSolar = vSolar × chgA`, `pLoad = vBatt × disA`, `telemetry.v_solar`/`v_batt`, `deriveSolStatus(vSolar, ...)`/`deriveBatStatus(..., vBatt, ...)`

> ⚠️ ก่อนหน้านี้ v_solar คำนวณ **inline 3 จุด** (Serial print + logAgg + telemetry) การเพิ่ม offset จึงยกมาประกาศเป็นตัวแปร `vSolar` ก่อน แล้วแทนที่ inline ทั้ง 3 จุดด้วยตัวแปร — ทำให้ใส่ offset ที่จุดเดียวก็พอ

### 8.3 ผลการ build
```
RAM:   15.3% (50,128 / 327,680 B)
Flash: 15.7% (1,027,789 / 6,553,600 B)
========================= [SUCCESS] =========================
```

---

## 9. อัปเดต 2026-09-30 — Web UI Calibration & Configurable Periodic Auto-Reboot

เพิ่มแถบตั้งค่าในหน้า Web UI Configuration Portal (`setup_web.h`) เพื่อให้ผู้ใช้สามารถปรับแต่งค่า Voltage Offset และระบบรีบูตอัตโนมัติได้โดยตรงผ่านเบราว์เซอร์:

### 9.1 แท็บใหม่: `⚙️ Calibration & System` (`#tab_cal` / `#panel_cal`)
1. **Voltage Offset Calibration (`v_solar` & `v_batt`)**:
   - ช่องกรอก `Solar Panel Voltage Offset (V)` และ `Battery Voltage Offset (V)`
   - สเต็ป `0.001`, ขอบเขตจำกัด `±1.000 V`
   - **Hot-Apply**: เมื่อกดบันทึก ค่าจะถูกเซฟลง NVS namespace `"solarcal"` (`vsOfs`, `vbOfs`) และอัปเดตตัวแปรใน RAM ทันทีผ่านแฟล็ก `g_calChanged` **โดยไม่ต้องรีบูตบอร์ด** ทำให้การวัดและส่งขึ้น ThingsBoard มีผลตามค่าชดเชยใหม่ภายใน 5 วินาที
   - ซิงก์ร่วมกับคำสั่ง Serial `V`/`v`/`P`/`p` แบบ 100% สองทาง
2. **Automated Maintenance (Periodic Auto-Reboot Control)**:
   - Checkbox สวิตช์: `Enable Periodic Auto-Reboot` เพื่อเปิดหรือปิดระบบรีบูตอัตโนมัติ
   - ช่องกรอก: `Reboot Interval (Hours)` กำหนดจำนวนชั่วโมงที่จะให้บอร์ดรีบูตตัวเอง (1 ถึง 720 ชม., ค่าเริ่มต้น 24 ชม.)
   - บันทึกลง NVS namespace `"app_cfg"` (`rb_en`, `rb_h`)
   - ปรับ `serviceDailyRestart()` ให้ตรวจสอบตามเงื่อนไข:
     - หากปิดสวิตช์ (`reboot_en = false`) $\rightarrow$ ไม่มีการรีบูตอัตโนมัติ
     - หากเปิดและตั้ง 24 ชม. ร่วมกับ NTP sync $\rightarrow$ รีบูตตอน 00:01 น. ตามรอบวัน
     - หากเปิดและ Uptime ครบตามจำนวนชั่วโมงที่กำหนด $\rightarrow$ ทำการ Flush NVS (SoC, SoH, CSV row) แล้วรีบูต
3. **การแจ้งเตือนผลการบันทึก**:
   - เมื่อกดปุ่ม **"Save Calibration & System"** ระบบจะส่ง POST ไปยัง `/save_cal` แล้วรีไดเรกต์กลับมาพร้อมแถบข้อความแจ้งเตือนสีเขียว:
     `✔ Calibration & System settings saved successfully! Changes are live.`

### 9.2 ผลการ build ล่าสุด
```
RAM:   15.3% (50,128 / 327,680 B)
Flash: 15.8% (1,032,613 / 6,553,600 B)
========================= [SUCCESS] =========================
```

บันทึก ณ วันที่ 30 กันยายน 2026 โดย AI Assistant
