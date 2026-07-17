import serial
import serial.tools.list_ports
import datetime
import time
import sys

def find_esp32_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        if any(x in p.description for x in ['CP210', 'CH340', 'UART', 'USB Serial']):
            return p.device
    return None

def sync_time(port=None, baud=115200):
    if port is None:
        port = find_esp32_port()
        if port is None:
            print("ERROR: Could not auto-detect ESP32 port.")
            print("Run: python set_rtc.py COM3  (replace COM3 with your port)")
            sys.exit(1)

    print(f"Connecting to ESP32 on {port}...")
    try:
        ser = serial.Serial(port, baud, timeout=3)
    except Exception as e:
        print(f"ERROR: Could not open port: {e}")
        sys.exit(1)

    time.sleep(2)  # wait for ESP32 to boot

    # Get current local time
    now = datetime.datetime.now()
    # Format: T2026,7,10,22,30,0\n
    cmd = f"T{now.year},{now.month},{now.day},{now.hour},{now.minute},{now.second}\n"
    print(f"Sending time: {now.strftime('%Y-%m-%d %H:%M:%S')}")

    ser.write(cmd.encode())
    time.sleep(0.5)

    # Read and print ESP32 response
    while ser.in_waiting:
        line = ser.readline().decode(errors='ignore').strip()
        if line:
            print(f"ESP32: {line}")

    ser.close()
    print("Done.")

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else None
    sync_time(port)