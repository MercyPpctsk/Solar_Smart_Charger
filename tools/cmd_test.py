# Send serial commands to the board and capture the reply.
# Usage: python cmd_test.py [t|d|E]   (default: t then d)
import serial, time, sys

port = "COM21"
cmds = sys.argv[1:] or ["t", "d"]
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 1
s.rts = False   # keep EN high: avoid resetting the S3 when opening the port
s.dtr = False
s.open()
time.sleep(0.5)
s.reset_input_buffer()
for c in cmds:
    s.write(c.encode())
    time.sleep(2.5)
data = s.read(30000)
s.close()
sys.stdout.write(data.decode("utf-8", "replace"))
