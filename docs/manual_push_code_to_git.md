# คู่มือขั้นตอนการ Push โค้ดขึ้น Git / GitHub
## (Manual: How to Push Code to Git)

คู่มือนี้จัดทำขึ้นสำหรับโปรเจกต์ **Solar Smart Charger (ESP32-S3-ETH)** เพื่อใช้เป็นแนวทางมาตรฐานในการบันทึกและส่งโค้ด (Push) ขึ้นสู่ GitHub Repository อย่างถูกต้อง ปลอดภัย และเป็นระบบ

- **Repository URL**: `https://github.com/MercyPpctsk/Solar_Smart_Charger.git`
- **Main Branch**: `main`

---

## 1. การเตรียมตัวและตั้งค่าครั้งแรก (Initial Setup)
*(หากโฟลเดอร์โปรเจกต์มีโฟลเดอร์ `.git` และเชื่อมต่อกับ GitHub อยู่แล้ว สามารถข้ามไปที่ [หัวข้อที่ 2](#2-ขั้นตอนการบันทึกและ-push-โค้ดประจำวัน-daily-workflow) ได้ทันที)*

### 1.1 ตรวจสอบผู้ใช้งาน Git บนเครื่อง
เปิด Terminal (PowerShell หรือ Command Prompt) แล้วตรวจสอบชื่อและอีเมล:
```bash
git config --get user.name
git config --get user.email
```
หากยังไม่ได้ตั้งค่า ให้กำหนดด้วยคำสั่ง:
```bash
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
```

### 1.2 เริ่มต้นสร้าง Local Git Repository ในโฟลเดอร์โปรเจกต์
เข้าไปที่โฟลเดอร์โปรเจกต์แล้วรันคำสั่ง:
```bash
git init
```

### 1.3 เชื่อมต่อ Local Repo เข้ากับ GitHub (Remote Origin)
```bash
git remote add origin https://github.com/MercyPpctsk/Solar_Smart_Charger.git
```

### 1.4 ดึงข้อมูล Branch และประวัติจาก GitHub
```bash
git fetch origin
```

### 1.5 เชื่อมโยง Branch `main` ในเครื่องให้ตรงกับบน GitHub
```bash
git branch main origin/main
git symbolic-ref HEAD refs/heads/main
git reset
```

---

## 2. ขั้นตอนการบันทึกและ Push โค้ดประจำวัน (Daily Workflow)

เมื่อมีการแก้ไขโค้ด ปรับแต่งฟังก์ชัน หรือเพิ่มเอกสารใหม่ ให้ปฏิบัติตามลำดับขั้นตอนดังนี้:

### ขั้นตอนที่ 1: ตรวจสอบสถานะไฟล์ (Check Status)
ใช้คำสั่งนี้เสมอเพื่อดูว่ามีไฟล์ใดบ้างที่มีการแก้ไข (`modified`), มีไฟล์ใหม่ (`untracked`), หรือมีไฟล์ที่ไม่พึงประสงค์หลุดเข้ามาหรือไม่:
```bash
git status
```

### ขั้นตอนที่ 2: ตรวจสอบความเปลี่ยนแปลงของโค้ด (Check Diff)
ตรวจสอบรายละเอียดบรรทัดโค้ดที่ถูกแก้ไข:
```bash
git diff
```

### ขั้นตอนที่ 3: เลือกไฟล์ที่ต้องการบันทึกเข้าสู่ Staging (Git Add)
- **กรณีเลือกเฉพาะไฟล์ที่ต้องการ:**
  ```bash
  git add src/main.cpp src/setup_web.h docs/UPDATE_2026-09-30.md
  ```
- **กรณีต้องการบันทึกไฟล์ที่มีการเปลี่ยนแปลงทั้งหมด (แนะนำ):**
  ```bash
  git add .
  ```
  *(ระบบจะคัดกรองตามไฟล์ `.gitignore` อัตโนมัติ โดยจะไม่ดึงไฟล์รหัสผ่าน `src/config.h` หรือไฟล์ build `.pio/` เข้าไป)*

### ขั้นตอนที่ 4: บันทึกประวัติการเปลี่ยนแปลง (Git Commit)
เขียนข้อความอธิบายการแก้ไขสั้นๆ แต่ได้ใจความว่ารอบนี้ทำอะไรไปบ้าง:
```bash
git commit -m "Add Web UI controls for voltage offset calibration and periodic auto-reboot"
```

### ขั้นตอนที่ 5: ส่งโค้ดขึ้น GitHub (Git Push)
ส่ง Commit จากเครื่อง Local ขึ้นไปยัง Remote Branch `main` บน GitHub:
```bash
git push origin main
```
*เมื่อคำสั่งทำงานสำเร็จ จะมีข้อความแสดง `main -> main` ถือเป็นอันเสร็จสิ้น*

---

## 3. สรุปคำสั่งลัดสำหรับการใช้งานทั่วไป (Quick Cheat Sheet)

```bash
# 1. เช็คสถานะ
git status

# 2. เตรียมไฟล์
git add .

# 3. บันทึกคำอธิบาย
git commit -m "ข้อความอธิบายสิ่งที่แก้ไข"

# 4. ส่งขึ้น GitHub
git push origin main
```

---

## 4. ข้อควรระวังและแนวทางปฏิบัติที่ดี (Best Practices)

> [!WARNING]
> **1. ห้าม Push ไฟล์รหัสผ่านหรือ Credentials เด็ดขาด:**  
> ไฟล์ `src/config.h` และ `tools/iqair/config.py` มีข้อมูล WiFi Password และ ThingsBoard Token ส่วนตัว ซึ่งถูกระบุไว้ใน `.gitignore` เรียบร้อยแล้ว **ห้ามใช้คำสั่ง `git add -f`** เพื่อฝืนดึงไฟล์เหล่านี้เข้าไปเด็ดขาด

> [!IMPORTANT]
> **2. ตรวจสอบการคอมไพล์ก่อน Push เสมอ:**  
> ควรสั่งรัน `pio run` หรือคอมไพล์ให้แน่ใจว่าเฟิร์มแวร์ไม่มี Error และไม่มี Warning ร้ายแรงก่อนทำการ Commit และ Push ขึ้น Repository

> [!TIP]
> **3. กรณีขึ้นข้อความเตือนเรื่อง LF / CRLF (บน Windows):**  
> ข้อความเตือน `warning: LF will be replaced by CRLF` เป็นเรื่องปกติของระบบจัดการขึ้นบรรทัดใหม่ระหว่าง Windows และ Linux ไม่มีผลเสียต่อโค้ด สามารถดำเนินงานต่อได้ตามปกติ
