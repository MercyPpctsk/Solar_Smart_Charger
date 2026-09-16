# Capture the boot banner: a short pio monitor owns the port and (like any
# USB-JTAG monitor) resets the S3 on open, so the FIRST lines are captured.
# Usage: python banner.py [seconds]   (default 14)
import subprocess, time, sys

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 14
pio = r"C:\Users\littl\AppData\Local\Programs\Python\Python313\Scripts\platformio.exe"
cmd = [pio, "device", "monitor", "-p", "COM21", "-b", "115200", "--quiet"]

with open("banner_out.txt", "w", encoding="utf-8") as f:
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                         cwd=r"c:\Ppctsk\ESP32-S3-ETH")
    time.sleep(secs)
    p.terminate()
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()

sys.stdout.write(open("banner_out.txt", encoding="utf-8").read())
