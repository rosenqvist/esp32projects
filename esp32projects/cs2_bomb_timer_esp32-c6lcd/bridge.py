"""
CS2 Bomb Timer SERIAL PC Bridge
Polls Omega's /luar endpoint, forwards bomb state over USB serial to ESP32.

Protocol (9-field CSV):
    ticking,defused,time_left,timer_length,being_defused,has_kit,hp_after,defuse_time_left,planting

Usage:  python bridge.py COM3
Requires: pip install pyserial requests
"""

import sys, time, serial, requests

OMEGA_URL = "http://127.0.0.1:8888/luar"
BAUD_RATE = 115200
POLL_RATE = 0.01

def main():
    if len(sys.argv) < 2:
        print("Usage: python bridge.py <COM_PORT>")
        sys.exit(1)

    port = sys.argv[1]
    print(f"[bridge] Opening {port} at {BAUD_RATE} baud...")

    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=0.1, dsrdtr=False, rtscts=False)
        ser.dtr = False
        ser.rts = False
    except serial.SerialException as e:
        print(f"[bridge] ERROR opening {port}: {e}")
        sys.exit(1)

    print(f"[bridge] Serial OK on {port}")
    print(f"[bridge] Polling {OMEGA_URL} every {POLL_RATE}s")
    print(f"[bridge] Press Ctrl+C to stop.\n")

    last_data = ""
    fail_count = 0
    send_count = 0

    while True:
        try:
            resp = requests.get(OMEGA_URL, timeout=0.5)
            if resp.status_code == 200:
                data = resp.text.strip()
                if data.startswith('"') and data.endswith('"'):
                    data = data[1:-1]

                parts = data.split(',')
                if len(parts) != 9:
                    fail_count += 1
                    if fail_count == 1 or fail_count % 20 == 0:
                        print(f"[bridge] Unexpected response ({len(parts)} fields): {data[:80]}")
                    time.sleep(POLL_RATE)
                    continue

                ser.write((data + "\n").encode("ascii"))
                send_count += 1

                if data != last_data:
                    print(f"[bridge] >> {data}")
                    last_data = data
                elif send_count % 30 == 0:
                    print(f"[bridge] (heartbeat) {data}")
                fail_count = 0
            else:
                fail_count += 1
                if fail_count == 1:
                    print(f"[bridge] HTTP {resp.status_code}")

        except requests.exceptions.ConnectionError:
            fail_count += 1
            if fail_count == 1:
                print("[bridge] Omega not reachable")
            elif fail_count % 20 == 0:
                print("[bridge] Still waiting...")
        except requests.exceptions.Timeout:
            fail_count += 1
        except serial.SerialException as e:
            print(f"[bridge] Serial lost: {e}")
            break
        except KeyboardInterrupt:
            print("\n[bridge] Stopped.")
            break

        time.sleep(POLL_RATE)

    ser.close()

if __name__ == "__main__":
    main()
