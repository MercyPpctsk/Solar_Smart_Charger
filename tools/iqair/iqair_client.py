# iqair_client.py
"""
ดึงข้อมูลจาก IQAir Station validated-data endpoint (public, ไม่ต้องใช้ API key)
วนลูปดึงข้อมูลตาม interval ที่กำหนด — ข้อมูลจริงอัปเดตแค่ระดับรายชั่วโมง
จึงไม่มีประโยชน์ที่จะ poll ถี่กว่านั้น
"""

import requests
import time
import signal
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Optional

from config import IQAIR_STATION_URL

POLL_INTERVAL_SEC = 30 * 60   # 30 นาที — ปรับได้ แต่ไม่แนะนำให้ถี่กว่า ~15 นาที
REQUEST_TIMEOUT_SEC = 10


@dataclass
class IQAirData:
    aqi_us: int = 0
    pm25: float = 0.0
    temp_c: float = 0.0
    humidity: float = 0.0
    pressure: float = 0.0
    ts: str = ""
    valid: bool = False
    last_fetch: float = 0.0


_running = True


def _handle_sigint(signum, frame):
    global _running
    print("\n[IQAir] received stop signal, shutting down after current cycle...")
    _running = False


def fetch_iqair(url: str, timeout: int = REQUEST_TIMEOUT_SEC) -> Optional[IQAirData]:
    try:
        resp = requests.get(url, timeout=timeout)
    except requests.exceptions.RequestException as e:
        print(f"[IQAir] request failed: {e}")
        return None

    if resp.status_code != 200:
        print(f"[IQAir] HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    try:
        payload = resp.json()
        current = payload["current"]
    except (ValueError, KeyError) as e:
        print(f"[IQAir] unexpected response: {e}")
        return None

    return IQAirData(
        aqi_us=current.get("aqius", 0),
        pm25=current.get("pm25", {}).get("concentration", 0.0),
        temp_c=current.get("temperature", 0.0),
        humidity=current.get("humidity", 0.0),
        pressure=current.get("pressure", 0.0),
        ts=current.get("ts", ""),
        valid=True,
        last_fetch=time.time(),
    )


def run_loop(url: str, interval_sec: int = POLL_INTERVAL_SEC):
    signal.signal(signal.SIGINT, _handle_sigint)

    last_ts_seen = None
    print(f"[IQAir] starting poll loop, interval={interval_sec}s (Ctrl+C to stop)")

    while _running:
        cycle_start = time.time()
        now_str = datetime.now(timezone.utc).strftime("%H:%M:%S UTC")

        data = fetch_iqair(url)

        if data and data.valid:
            # เตือนถ้า timestamp จากฝั่ง IQAir ไม่ขยับ = device ยังไม่ push ข้อมูลใหม่
            stale_flag = " (STALE - ยังเป็นค่าเดิม)" if data.ts == last_ts_seen else ""
            last_ts_seen = data.ts

            print(
                f"[{now_str}] ts={data.ts}{stale_flag} "
                f"AQI={data.aqi_us} PM2.5={data.pm25}µg/m3 "
                f"T={data.temp_c}°C RH={data.humidity}% P={data.pressure}hPa"
            )
        else:
            print(f"[{now_str}] fetch failed, will retry next cycle")

        # หัก elapsed time ออกจาก interval กัน drift สะสม
        elapsed = time.time() - cycle_start
        sleep_time = max(0, interval_sec - elapsed)

        # sleep แบบ interruptible เพื่อให้ Ctrl+C ตอบสนองไว ไม่ต้องรอครบ interval
        slept = 0
        while slept < sleep_time and _running:
            step = min(1, sleep_time - slept)
            time.sleep(step)
            slept += step

    print("[IQAir] stopped.")


if __name__ == "__main__":
    interval = POLL_INTERVAL_SEC
    if len(sys.argv) > 1:
        try:
            interval = int(sys.argv[1])
        except ValueError:
            print(f"invalid interval arg '{sys.argv[1]}', using default {POLL_INTERVAL_SEC}s")

    run_loop(IQAIR_STATION_URL, interval)