import time
import serial

port = 'COM21'
ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
# Terminal-style: DTR asserted so HWCDC streams; RTS low (RTS toggling => download mode)
ser.dtr = True
ser.rts = False
ser.open()
ser.dtr = True
ser.rts = False

print(f'--- Continuously reading {port} (DTR=1,RTS=0) [Press Ctrl+C to stop] ---\n')

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
                    print(text)
        else:
            time.sleep(0.02)

except KeyboardInterrupt:
    print('\n--- Stopped by user ---')
finally:
    ser.close()
    print('Port closed.')