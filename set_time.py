import serial
import serial.tools.list_ports
import datetime
import time
import sys


def find_esp32_port():
    ports = serial.tools.list_ports.comports()

    for p in ports:
        description = p.description or ""
        if any(x in description for x in ["CP210", "CH340", "UART", "USB Serial"]):
            return p.device

    return None


def sync_time(port=None, baud=115200):
    if port is None:
        port = find_esp32_port()

        if port is None:
            print("ERROR: Could not auto-detect ESP32 port.")
            print("Run: python set_rtc.py COM3")
            print("(replace COM3 with your ESP32 port)")
            sys.exit(1)

    print(f"Connecting to ESP32 on {port}...")

    try:
        ser = serial.Serial(port, baud, timeout=3)
    except Exception as e:
        print(f"ERROR: Could not open port: {e}")
        sys.exit(1)

    # Allow ESP32 to boot/reset after opening the serial connection.
    time.sleep(2)

    # Use UTC for the RTC.
    now = datetime.datetime.now(datetime.timezone.utc)

    # ESP32 expects:
    # TYYYY,MM,DD,HH,MM,SS
    cmd = (
        f"T{now.year},{now.month},{now.day},"
        f"{now.hour},{now.minute},{now.second}\n"
    )

    print(f"Sending UTC time: {now.strftime('%Y-%m-%d %H:%M:%S UTC')}")

    ser.write(cmd.encode("ascii"))
    ser.flush()

    time.sleep(0.5)

    # Read and print ESP32 response.
    while ser.in_waiting:
        line = ser.readline().decode(errors="ignore").strip()

        if line:
            print(f"ESP32: {line}")

    ser.close()

    print("RTC synchronization complete.")


if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else None
    sync_time(port)