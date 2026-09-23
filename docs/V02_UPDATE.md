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
