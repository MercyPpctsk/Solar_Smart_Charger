import time
import serial

port = 'COM11'
ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.open()
# Set both control lines low (inactive)
ser.dtr = False
ser.rts = False
time.sleep(0.1)

# Quick reset pulse on EN to restart firmware and capture boot logs
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)

print(f'--- Continuously reading {port} [Press Ctrl+C to stop] ---\n')

buffer = b''

try:
    while True:
        n = ser.in_waiting
        if n:
            buffer += ser.read(n)
            # ตัดประมวลผลทีละบรรทัดเมื่อเจอ \n
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                text = line.decode('utf-8', 'replace').strip()
                if text:
                    print(text, flush=True)
        else:
            time.sleep(0.02)

except KeyboardInterrupt:
    print('\n--- Stopped by user ---')
finally:
    ser.close()
    print('Port closed.')