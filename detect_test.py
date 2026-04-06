#!/usr/bin/env python3
"""
Replicates the C++ batch-delogo detection logic in Python using OpenCV.
Matches exactly: cv::imread (no flags = BGR, alpha dropped), TM_CCOEFF_NORMED,
multi-scale (0.9, 0.95, 1.0, 1.05, 1.1), quadrant-based search regions.
"""

import cv2
import numpy as np
import json
import sys

# ── Config ────────────────────────────────────────────────────────────────────
VIDEO      = "videos_to_process/04-01-2020-todosApplicationPart5-pjts-Hindi-V2-720p-30s-60s.mp4"
CONFIG     = "video_layouts.json"
REF_DIR    = "reference_images"
LAYOUT_ID  = "nxtwave_720p"
SAMPLE_EVERY = 30          # match C++ default sample interval
USE_MULTI_SCALE = True     # match C++ --multi-scale flag
MULTI_SCALES = [0.9, 0.95, 1.0, 1.05, 1.1]

# ── Helpers ───────────────────────────────────────────────────────────────────

def quadrant_to_region(q, frame_w, frame_h):
    """Convert quadrant number (1-4) to (x, y, w, h) — mirrors C++ logic."""
    hw, hh = frame_w // 2, frame_h // 2
    return {
        1: (0,   0,   hw,  hh),   # top-left
        2: (hw,  0,   hw,  hh),   # top-right
        3: (0,   hh,  hw,  hh),   # bottom-left
        4: (hw,  hh,  hw,  hh),   # bottom-right
    }[q]


def detect_logo(frame, template, region, threshold, use_multi_scale=True):
    """
    Exact Python replica of C++ detect_logo_in_frame().
    Returns (found, x, y, confidence).
    """
    rx, ry, rw, rh = region
    rx = max(0, rx);  ry = max(0, ry)
    rw = min(rw, frame.shape[1] - rx)
    rh = min(rh, frame.shape[0] - ry)
    if rw <= 0 or rh <= 0:
        return False, 0, 0, -1.0

    search_area = frame[ry:ry+rh, rx:rx+rw]
    best_val = -1.0
    best_loc = (0, 0)

    if use_multi_scale:
        for scale in MULTI_SCALES:
            tw = int(template.shape[1] * scale)
            th = int(template.shape[0] * scale)
            if tw < 5 or th < 5 or tw > rw or th > rh:
                continue
            scaled = cv2.resize(template, (tw, th), interpolation=cv2.INTER_LINEAR)
            result = cv2.matchTemplate(search_area, scaled, cv2.TM_CCOEFF_NORMED)
            _, max_val, _, max_loc = cv2.minMaxLoc(result)
            if max_val > best_val:
                best_val = max_val
                best_loc = max_loc
    else:
        result = cv2.matchTemplate(search_area, template, cv2.TM_CCOEFF_NORMED)
        _, best_val, _, best_loc = cv2.minMaxLoc(result)

    found = best_val >= threshold
    abs_x = rx + best_loc[0]
    abs_y = ry + best_loc[1]
    return found, abs_x, abs_y, best_val


# ── Load config ───────────────────────────────────────────────────────────────
with open(CONFIG) as f:
    config = json.load(f)

layout = next(l for l in config["layouts"] if l["id"] == LAYOUT_ID)
frame_w = layout["resolution"]["width"]
frame_h = layout["resolution"]["height"]

# Build detection list (mirrors C++ DetectionState population)
detections = []
for det in layout["detections"]:
    tmpl = cv2.imread(f'{REF_DIR}/{det["reference_image"]}')  # no flags = BGR, alpha dropped (same as C++)
    if tmpl is None:
        print(f'  [WARN] Could not load {det["reference_image"]}')
        continue

    if "search_quadrant" in det:
        region = quadrant_to_region(det["search_quadrant"], frame_w, frame_h)
    elif "search_region" in det:
        sr = det["search_region"]
        region = (sr["x"], sr["y"], sr["width"], sr["height"])
    else:
        region = (0, 0, frame_w, frame_h)

    detections.append({
        "name":      det["name"],
        "ref_image": det["reference_image"],
        "template":  tmpl,
        "region":    region,
        "threshold": det["match_threshold"],
    })
    rx, ry, rw, rh = region
    print(f'  Loaded: {det["name"]:<30}  template={tmpl.shape[1]}x{tmpl.shape[0]}'
          f'  search_area={rw}x{rh}  threshold={det["match_threshold"]}')

# ── Open video ────────────────────────────────────────────────────────────────
cap = cv2.VideoCapture(VIDEO)
total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
fps          = cap.get(cv2.CAP_PROP_FPS)
print(f'\nVideo: {total_frames} frames @ {fps}fps  ({total_frames/fps:.1f}s)\n')
print(f'{"Frame":>6}  {"Detection":<30}  {"Conf":>6}  {"Thresh":>6}  {"Found":>5}  {"Position"}')
print('-' * 80)

# ── Frame loop ────────────────────────────────────────────────────────────────
frame_num = 0
while frame_num < total_frames:
    cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
    ret, frame = cap.read()
    if not ret:
        break

    for det in detections:
        found, x, y, conf = detect_logo(
            frame, det["template"], det["region"],
            det["threshold"], USE_MULTI_SCALE
        )
        status = "YES" if found else "no"
        pos    = f'({x},{y})' if found else '-'
        print(f'{frame_num:>6}  {det["name"]:<30}  {conf:>6.3f}  {det["threshold"]:>6.2f}  {status:>5}  {pos}')

    frame_num += SAMPLE_EVERY

cap.release()
print('\nDone.')
