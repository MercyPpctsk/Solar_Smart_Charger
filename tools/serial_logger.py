"""Background serial logger for the ESP32-S3-ETH solar charger.

Usage:
    python tools/serial_logger.py [seconds] [port]

- seconds : capture duration, default 3600 (1 hour)
- port    : serial port, default COM21

Writes every received line to tools/serial_log.txt, prefixed with an
ISO-8601 timestamp. Logger status lines are prefixed [LOGGER].
Flushes after every line so captured data is preserved if the process
is killed or the board is unplugged. Does NOT touch DTR/RTS, so the
board keeps running undisturbed (no reset).

To stop early, create a file named tools/serial_log_STOP (the logger
checks for it each loop and exits cleanly). Otherwise it runs for the
full duration.
"""
import os
import sys
import time
import serial
from datetime import datetime

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 3600.0
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM21"
BAUD = 115200

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(SCRIPT_DIR, "serial_log.txt")
STOP_FILE = os.path.join(SCRIPT_DIR, "serial_log_STOP")


def iso_now():
    return datetime.now().isoformat(timespec="seconds")


def writeln(f, msg):
    f.write(msg + "\n")
    f.flush()


def open_port():
    return serial.Serial(PORT, BAUD, timeout=1)


# remove a stale stop file from a previous run
if os.path.exists(STOP_FILE):
    try:
        os.remove(STOP_FILE)
    except OSError:
        pass

line_count = 0
ser = None
with open(LOG_PATH, "w", encoding="utf-8") as f:
    writeln(f, f"[LOGGER] start {iso_now()} port={PORT} baud={BAUD} duration={DURATION}s")
    for attempt in range(1, 6):
        try:
            ser = open_port()
            break
        except Exception as e:
            writeln(f, f"[LOGGER] open attempt {attempt} failed: {e}")
            time.sleep(2)
    if ser is None:
        writeln(f, f"[LOGGER] FATAL could not open {PORT} after 5 attempts; exiting")
        sys.exit(1)
    writeln(f, f"[LOGGER] port opened; capturing...")

    end = time.time() + DURATION
    last_progress = time.time()
    buf = b""
    try:
        while time.time() < end:
            if os.path.exists(STOP_FILE):
                writeln(f, f"[LOGGER] stop file detected; exiting cleanly")
                break
            try:
                chunk = ser.read(4096)
            except Exception as e:
                writeln(f, f"[LOGGER] read error: {e}; reconnecting")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2)
                try:
                    ser = open_port()
                    writeln(f, f"[LOGGER] reconnected")
                except Exception as e2:
                    writeln(f, f"[LOGGER] reconnect failed: {e2}")
                continue
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if text:
                        writeln(f, f"[{iso_now()}] {text}")
                        line_count += 1
            if time.time() - last_progress > 60:
                remaining = max(0, int(end - time.time()))
                writeln(f, f"[LOGGER] progress: {line_count} lines captured, {remaining}s remaining")
                last_progress = time.time()
    except KeyboardInterrupt:
        writeln(f, f"[LOGGER] interrupted")
    finally:
        try:
            ser.close()
        except Exception:
            pass
        writeln(f, f"[LOGGER] end {iso_now()} total_lines={line_count}")