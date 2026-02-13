# Hybrid Processing: Automatic Moving vs Static Logo Detection

## Overview

The system now **automatically detects** whether logos are moving or static and processes them accordingly:
- **Static logos** → Fast FFmpeg processing
- **Moving logos** → Frame-by-frame tracking
- **Mixed** → Frame-by-frame for all (with recommendation to separate)

## How It Works

### 1. Detection Phase
When you use `--track-moving`, the system:
1. Samples frames at specified interval (e.g., every frame with `--sample-interval 1`)
2. Tracks logo positions across frames
3. **Analyzes movement**: Calculates average position change between frames

### 2. Movement Classification
```cpp
bool is_logo_moving(positions, threshold = 5.0 pixels)
```

For each detected segment:
- If average movement > 5 pixels/frame → **MOVING**
- If average movement ≤ 5 pixels/frame → **STATIC**

### 3. Processing Decision

| Scenario | Processing Method | Speed |
|----------|------------------|-------|
| All logos static | FFmpeg overlays | ⚡ Very Fast |
| All logos moving | Frame-by-frame | 🐢 Slow but accurate |
| Mixed (some moving, some static) | Frame-by-frame for all | 🐢 Slow |

## Usage

### Automatic Hybrid Mode

```bash
./src/batch-delogo/batch-delogo \
  --input-folder ./videos_to_process \
  --output-folder ./processed_videos \
  --config ./video_layouts.json \
  --auto-detect \
  --track-moving \
  --sample-interval 1
```

**What happens:**
1. System detects all logos with frame-by-frame sampling
2. Analyzes which logos are moving vs static
3. Automatically chooses best processing method
4. Shows classification in output:
   ```
   [Logo Bottom Right] Final segment: frames 0-1780 [MOVING]
   [IB Hubs Author] Final segment: frames 0-1780 [STATIC]
   ```

### For Best Performance

If you have **mixed logos** (some moving, some static), the system will use slow frame-by-frame processing for everything. To optimize:

#### Option 1: Separate Configs

Create two config files:

**`video_layouts_static.json`** - Only static logos:
```json
{
  "layouts": [{
    "detections": [
      {
        "name": "Static Logo Top Right",
        "reference_image": "static_logo.png",
        ...
      }
    ]
  }]
}
```

**`video_layouts_moving.json`** - Only moving logos:
```json
{
  "layouts": [{
    "detections": [
      {
        "name": "Sliding Logo",
        "reference_image": "moving_logo.png",
        ...
      }
    ]
  }]
}
```

Then process in two passes:

```bash
# Pass 1: Fast processing for static logos
./src/batch-delogo/batch-delogo \
  --input-folder ./videos_to_process \
  --output-folder ./temp_output \
  --config ./video_layouts_static.json \
  --auto-detect

# Pass 2: Slow processing for moving logos on the output from pass 1
./src/batch-delogo/batch-delogo \
  --input-folder ./temp_output \
  --output-folder ./processed_videos \
  --config ./video_layouts_moving.json \
  --auto-detect \
  --track-moving \
  --sample-interval 1
```

#### Option 2: Adjust Movement Threshold

If logos are classified incorrectly, you can adjust the threshold in the code:

```cpp
// In detect_moving_logos_in_video()
bool moving = is_logo_moving(positions, 10.0);  // Increase from 5.0 to 10.0
```

Higher threshold = fewer logos classified as "moving"

## Sample Intervals for Moving Logos

The `--sample-interval` determines detection accuracy for moving logos:

| Interval | Detection Speed | Tracking Accuracy | Use Case |
|----------|----------------|-------------------|----------|
| 30 (default) | ⚡ Very Fast | ❌ Poor for slides | Static logos only |
| 10 | 🚀 Fast | ⚠️ May miss fast slides | Slow movements |
| 3 | 🏃 Moderate | ✅ Good for most slides | Recommended for moving |
| 1 | 🐢 Slow | ✅✅ Perfect tracking | Fast slides, critical videos |

### Recommended Settings

**For videos with slide transitions:**
```bash
--track-moving --sample-interval 3
```

**For videos with fast animations:**
```bash
--track-moving --sample-interval 1
```

**For static logos (no movement):**
```bash
# Don't use --track-moving at all!
```

## Understanding the Output

### Detection Phase Output

```
[Logo Bottom Right] Final segment: frames 0-1780 with 175 tracked positions [MOVING]
```

- **175 tracked positions** = Logo detected in 175 frames
- **[MOVING]** = Average movement > 5 pixels/frame
- Will use frame-by-frame processing

```
[IB Hubs Author] Final segment: frames 0-1780 with 1780 tracked positions [STATIC]
```

- **1780 tracked positions** = Logo detected in every frame
- **[STATIC]** = Logo stays in same position
- Would use FFmpeg if no moving logos present

### Processing Decision Output

```
Detected mix of moving and static logos - using frame-by-frame processing
Note: For better performance, separate moving and static logos into different configs
```

This tells you that you could optimize by separating configs.

## Performance Comparison

### Example: 1-minute video (1800 frames, 1920x1080)

| Method | Detection Time | Processing Time | Total Time |
|--------|---------------|-----------------|------------|
| Static only (no --track-moving) | ~5s | ~30s | **~35s** |
| Moving (--sample-interval 30) | ~30s | ~5min | **~5.5min** |
| Moving (--sample-interval 3) | ~4min | ~5min | **~9min** |
| Moving (--sample-interval 1) | ~12min | ~5min | **~17min** |

**Hybrid approach (separate configs):**
- Static pass: ~35s
- Moving pass on result: ~9min (interval 3)
- **Total: ~10min** (vs 17min for all-moving)

## Troubleshooting

### Logo classified as STATIC but is actually moving

**Problem:** Movement is too small (< 5 pixels/frame)

**Solution:** Lower the threshold or use `--sample-interval 1` for better detection

### Logo classified as MOVING but is actually static

**Problem:** Jitter or noise in detection causing false movement

**Solution:** Increase threshold or improve reference image quality

### Old logo still visible during slides

**Problem:** Sample interval too large, missing intermediate positions

**Solution:** Use `--sample-interval 1` or `--sample-interval 3`

### Processing too slow

**Problem:** Using frame-by-frame for everything

**Solutions:**
1. Separate moving and static logos into different configs
2. Use larger sample interval (trade accuracy for speed)
3. Process only the sections with moving logos

## Future Enhancements

Potential improvements:
1. **Segment-based hybrid processing** - Use FFmpeg for static segments, frame-by-frame only for moving segments within same video
2. **Adaptive sampling** - Automatically increase sampling rate during detected movement
3. **Motion prediction** - Interpolate positions during detection failures
4. **Parallel processing** - Process multiple segments simultaneously

## Summary

The hybrid system gives you the best of both worlds:
- ⚡ **Fast processing** for static logos (FFmpeg)
- 🎯 **Accurate tracking** for moving logos (frame-by-frame)
- 🤖 **Automatic detection** of which is which

Just use `--track-moving` and the system handles the rest!

