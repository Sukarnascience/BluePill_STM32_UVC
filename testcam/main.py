#!/usr/bin/env python3
"""
view_uvc.py — View the BeeComposite UVC camera stream
Works on Windows, Linux, macOS.

Requirements:
    pip install opencv-python numpy

Usage:
    python view_uvc.py            # auto-detect camera
    python view_uvc.py --index 1  # use camera index 1
    python view_uvc.py --info     # list all cameras
"""

import cv2
import numpy as np
import argparse
import sys
import time

# ── colour palette for OSD ──────────────────────────────────
C_WHITE  = (255, 255, 255)
C_BLACK  = (0,   0,   0)
C_GREEN  = (0,   220, 80)
C_YELLOW = (0,   220, 220)
C_RED    = (0,   60,  220)
C_BLUE   = (200, 120, 0)
C_GRAY   = (160, 160, 160)


def list_cameras(max_index=8):
    """Return list of (index, name) for all available cameras."""
    found = []
    for i in range(max_index):
        cap = cv2.VideoCapture(i, cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_V4L2)
        if cap.isOpened():
            w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            found.append((i, f"Camera {i}  ({w}×{h})"))
            cap.release()
    return found


def find_beecamera(max_index=8):
    """Try to find the BeeComposite camera by checking resolution 176x144."""
    for i in range(max_index):
        cap = cv2.VideoCapture(i, cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_V4L2)
        if cap.isOpened():
            w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            if w == 176 and h == 144:
                cap.release()
                return i
            cap.release()
    return None


def draw_osd(frame, fps, frame_count, w, h, status):
    """Draw overlay info on frame."""
    out = frame.copy()
    # Semi-transparent top bar
    overlay = out.copy()
    cv2.rectangle(overlay, (0, 0), (out.shape[1], 36), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.65, out, 0.35, 0, out)

    cv2.putText(out, "BeeComposite UVC  |  BeeBotix", (8, 22),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, C_GREEN, 1, cv2.LINE_AA)
    cv2.putText(out, f"FPS:{fps:5.1f}  {w}x{h}  Frame:{frame_count:6d}",
                (out.shape[1] - 320, 22),
                cv2.FONT_HERSHEY_SIMPLEX, 0.48, C_YELLOW, 1, cv2.LINE_AA)

    # Status bar bottom
    cv2.rectangle(overlay, (0, out.shape[0] - 24), (out.shape[1], out.shape[0]),
                  (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.65, out, 0.35, 0, out)
    cv2.putText(out, status, (8, out.shape[0] - 7),
                cv2.FONT_HERSHEY_SIMPLEX, 0.42, C_GRAY, 1, cv2.LINE_AA)

    return out


def run_viewer(index):
    backend = cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_V4L2
    cap = cv2.VideoCapture(index, backend)

    if not cap.isOpened():
        print(f"[ERROR] Cannot open camera index {index}")
        sys.exit(1)

    # Request 176x144 — our UVC resolution
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  176)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 144)
    cap.set(cv2.CAP_PROP_FPS, 5)

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = cap.get(cv2.CAP_PROP_FPS)

    print(f"[INFO] Opened camera {index}:  {w}×{h}  @ {actual_fps:.0f} fps")
    print("[INFO] Press  Q  to quit,  S  to save snapshot,  Z  to zoom 4×")

    # Scale up for display (176×144 is tiny on modern monitors)
    SCALE = 4
    win_name = "BeeComposite UVC — BeeBotix"
    cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win_name, w * SCALE, h * SCALE + 60)

    frame_count = 0
    fps_display = 0.0
    t_fps = time.time()
    zoom = False
    status = f"Camera {index}  {w}×{h}  YUY2 → BGR  |  Q=quit  S=snapshot  Z=zoom"

    while True:
        ret, frame = cap.read()
        if not ret:
            print("[WARN] Frame read failed, retrying...")
            time.sleep(0.05)
            continue

        frame_count += 1

        # FPS calculation
        now = time.time()
        if now - t_fps >= 1.0:
            fps_display = frame_count / (now - t_fps + 1e-9) if frame_count == 1 else fps_display
            t_fps = now

        # Zoom mode: show centre crop ×2
        if zoom:
            cy, cx = frame.shape[0] // 2, frame.shape[1] // 2
            crop = frame[cy//2:cy+cy//2, cx//2:cx+cx//2]
            display = cv2.resize(crop, (w * SCALE, h * SCALE), interpolation=cv2.INTER_NEAREST)
        else:
            display = cv2.resize(frame, (w * SCALE, h * SCALE), interpolation=cv2.INTER_NEAREST)

        # Recalculate fps per second
        elapsed = time.time() - t_fps + 1e-6
        fps_display = frame_count / max(elapsed, 0.001)
        # Reset every second
        if time.time() - t_fps > 1.0:
            frame_count = 0
            t_fps = time.time()

        display = draw_osd(display, fps_display, frame_count, w, h, status)

        cv2.imshow(win_name, display)
        key = cv2.waitKey(1) & 0xFF

        if key == ord('q') or key == 27:
            break
        elif key == ord('s'):
            fname = f"snapshot_{int(time.time())}.png"
            cv2.imwrite(fname, frame)
            print(f"[INFO] Saved {fname}")
            status = f"Saved {fname}"
        elif key == ord('z'):
            zoom = not zoom
            status = f"Zoom {'ON' if zoom else 'OFF'}  |  Q=quit  S=snapshot  Z=zoom"

    cap.release()
    cv2.destroyAllWindows()
    print("[INFO] Done.")


def main():
    parser = argparse.ArgumentParser(description="BeeComposite UVC Viewer")
    parser.add_argument("--index", type=int, default=None,
                        help="Camera index (0,1,2...)")
    parser.add_argument("--info", action="store_true",
                        help="List all cameras and exit")
    args = parser.parse_args()

    if args.info:
        cams = list_cameras()
        if not cams:
            print("No cameras found.")
        else:
            print("Available cameras:")
            for idx, name in cams:
                print(f"  [{idx}] {name}")
        return

    index = args.index
    if index is None:
        auto = find_beecamera()
        if auto is not None:
            print(f"[INFO] Auto-detected BeeComposite at index {auto}")
            index = auto
        else:
            print("[INFO] BeeComposite not found by resolution, trying index 0")
            index = 0

    run_viewer(index)


if __name__ == "__main__":
    main()