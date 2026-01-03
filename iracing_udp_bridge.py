import argparse
import csv
import socket
import time
from datetime import datetime

import irsdk  # from pyirsdk

PC_IP = "192.168.1.80"       # unused, ok to keep
ESP32_IP = "192.168.1.60"    # must match your ESP sketch
ESP32_PORT = 5005

SEND_HZ = 20.0
LOG_PATH = f"iracing_telemetry_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"


def ir_get(ir, key, default=0.0):
    try:
        return ir[key]
    except Exception:
        return default


def ir_get_idx(ir, key, idx, default=0.0):
    """Safely read an indexed (CarIdx...) array value."""
    try:
        arr = ir[key]
        if isinstance(arr, (list, tuple)) and 0 <= idx < len(arr):
            return arr[idx]
        return default
    except Exception:
        return default


def norm_pct(x):
    """FuelLevelPct is usually 0..1. Handle 0..100 just in case."""
    try:
        x = float(x)
    except Exception:
        return 0.0
    if x > 1.5:
        x = x / 100.0
    if x < 0.0:
        x = 0.0
    if x > 1.0:
        x = 1.0
    return x


def run(live_only: bool):
    ir = irsdk.IRSDK()
    ir.startup()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    seq = 0
    last_print = 0.0
    period = 1.0 / SEND_HZ

    writer = None
    f = None

    try:
        if not live_only:
            f = open(LOG_PATH, "w", newline="", encoding="utf-8")
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "ts_unix", "seq",
                    "rpm", "gear", "throttle", "brake", "steer_norm",
                    "fuel_l", "fuel_pct", "speed_kmh",
                    "lap", "pos", "class_pos",
                    "lap_cur", "lap_last", "lap_best",
                    "is_replay",
                ],
            )
            writer.writeheader()
            print(f"[+] Logging to: {LOG_PATH}")
        else:
            print("[+] Live mode: CSV logging disabled (-l)")

        print(f"[+] Sending UDP to {ESP32_IP}:{ESP32_PORT} @ {SEND_HZ:.1f} Hz")
        print("[i] Start iRacing and get in-car to see live values.")

        while True:
            if not ir.is_initialized:
                if time.time() - last_print > 1.0:
                    print("[...] Waiting for iRacing SDK (not initialized)")
                    last_print = time.time()
                time.sleep(0.5)
                ir.startup()
                continue

            # Replay indicator + player index (for CarIdx fallbacks)
            is_replay = int(ir_get(ir, "IsReplayPlaying", 0))
            player_idx = int(ir_get(ir, "PlayerCarIdx", 0))

            # Core telemetry
            rpm = float(ir_get(ir, "RPM", 0.0))
            gear = int(ir_get(ir, "Gear", 0))
            throttle = float(ir_get(ir, "Throttle", 0.0))   # 0..1
            brake = float(ir_get(ir, "Brake", 0.0))         # 0..1

            # Steering normalization
            steer_angle = float(ir_get(ir, "SteeringWheelAngle", 0.0))
            steer_max = float(ir_get(ir, "SteeringWheelAngleMax", 0.0))
            steer_norm = (steer_angle / steer_max) if steer_max not in (0.0, None) else 0.0

            # Fuel + speed
            fuel_l = float(ir_get(ir, "FuelLevel", 0.0))
            fuel_pct = norm_pct(ir_get(ir, "FuelLevelPct", 0.0))

            speed_ms = float(ir_get(ir, "Speed", 0.0))       # m/s
            speed_kmh = speed_ms * 3.6

            # Lap + position (prefer PlayerCar..., fallback to CarIdx... especially in replay)
            lap = int(ir_get(ir, "Lap", 0))
            pos = int(ir_get(ir, "PlayerCarPosition", 0))
            class_pos = int(ir_get(ir, "PlayerCarClassPosition", 0))

            if player_idx >= 0:
                if lap == 0:
                    lap = int(ir_get_idx(ir, "CarIdxLap", player_idx, lap))
                if pos == 0:
                    pos = int(ir_get_idx(ir, "CarIdxPosition", player_idx, pos))
                if class_pos == 0:
                    class_pos = int(ir_get_idx(ir, "CarIdxClassPosition", player_idx, class_pos))

            # Lap times (seconds)
            lap_cur = float(ir_get(ir, "LapCurrentLapTime", 0.0))
            lap_last = float(ir_get(ir, "LapLastLapTime", 0.0))
            lap_best = float(ir_get(ir, "LapBestLapTime", 0.0))

            # If replay has zeros, try per-car arrays
            if player_idx >= 0:
                if lap_last <= 0.0:
                    lap_last = float(ir_get_idx(ir, "CarIdxLastLapTime", player_idx, lap_last))
                if lap_best <= 0.0:
                    lap_best = float(ir_get_idx(ir, "CarIdxBestLapTime", player_idx, lap_best))

            # UDP payload (15 values):
            # seq,rpm,gear,thr,brk,steer,fuel_l,fuel_pct,speed_kmh,lap,pos,class_pos,lap_cur,lap_last,lap_best
            payload = (
                f"{seq},{rpm:.1f},{gear},"
                f"{throttle:.3f},{brake:.3f},{steer_norm:.3f},"
                f"{fuel_l:.3f},{fuel_pct:.4f},{speed_kmh:.2f},"
                f"{lap},{pos},{class_pos},"
                f"{lap_cur:.3f},{lap_last:.3f},{lap_best:.3f}"
            )
            sock.sendto(payload.encode("ascii"), (ESP32_IP, ESP32_PORT))

            # Optional CSV logging
            if writer is not None:
                writer.writerow({
                    "ts_unix": time.time(),
                    "seq": seq,
                    "rpm": rpm,
                    "gear": gear,
                    "throttle": throttle,
                    "brake": brake,
                    "steer_norm": steer_norm,
                    "fuel_l": fuel_l,
                    "fuel_pct": fuel_pct,
                    "speed_kmh": speed_kmh,
                    "lap": lap,
                    "pos": pos,
                    "class_pos": class_pos,
                    "lap_cur": lap_cur,
                    "lap_last": lap_last,
                    "lap_best": lap_best,
                    "is_replay": is_replay,
                })

            # Console status at 1 Hz
            now = time.time()
            if now - last_print >= 1.0:
                print(
                    f"seq={seq:7d} rpm={rpm:7.0f} gear={gear:2d} "
                    f"spd={speed_kmh:6.1f}kmh fuel={fuel_l:6.2f}L ({fuel_pct*100:5.1f}%) "
                    f"lap={lap:3d} pos={pos:3d} cls={class_pos:3d} "
                    f"cur={lap_cur:7.3f} last={lap_last:7.3f} best={lap_best:7.3f} "
                    f"thr={throttle:4.2f} brk={brake:4.2f} replay={is_replay}"
                )
                last_print = now

            seq += 1
            time.sleep(period)

    except KeyboardInterrupt:
        print("\n[!] Stopped by user.")
    finally:
        try:
            ir.shutdown()
        except Exception:
            pass
        try:
            sock.close()
        except Exception:
            pass
        if f is not None:
            f.close()


def main():
    parser = argparse.ArgumentParser(description="iRacing -> UDP (ESP32) + optional CSV logger")
    parser.add_argument(
        "-l", "--live",
        action="store_true",
        help="Live-only mode: do not write CSV to disk"
    )
    args = parser.parse_args()
    run(live_only=args.live)


if __name__ == "__main__":
    main()
