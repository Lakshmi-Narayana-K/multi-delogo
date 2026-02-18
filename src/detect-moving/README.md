# detect-moving

A fast CLI tool that checks whether a video contains **moving logos**. It samples frames, runs template matching against the detections defined in `video_layouts.json`, and tracks position changes across frames. As soon as a logo is confirmed to be moving, it prints `true` and exits immediately — no need to scan the entire video.

## Prerequisites

### System packages (Debian / Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y \
  g++ make pkg-config \
  libopencv-dev libjsoncpp-dev
```

> These are the **only** dependencies needed for `detect-moving`. The full `multi-delogo` project has additional dependencies (GTK, Boost, etc.) but this tool does not require them.

### Build the shared library

`detect-moving` links against the `libfilter-generator.a` library from the parent project. If you haven't built the full project yet, you need to build at least that library first:

```sh
# From the repo root
git clone https://github.com/Lakshmi-Narayana-K/multi-delogo.git
cd multi-delogo

# Install ALL build dependencies (needed for ./configure even though detect-moving only uses a subset)
sudo apt-get install -y autoconf automake autopoint gettext libtool \
  libglib2.0-dev libgtkmm-3.0-dev libgoocanvas-2.0-dev \
  libopencv-dev libjsoncpp-dev libboost-all-dev

./autogen.sh
./configure
make -C src/filter-generator   # builds libfilter-generator.a
```

## Build

```sh
cd src/detect-moving
make
```

This produces the `detect-moving` binary in the same directory.

To rebuild after changes:

```sh
make clean && make
```

## Usage

```
detect-moving --video PATH --config PATH [OPTIONS]
```

### Required arguments

| Argument | Description |
|----------|-------------|
| `--video PATH` | Path to the video file to check |
| `--config PATH` | Path to `video_layouts.json` (detection rules) |

### Optional arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--sample-interval N` | `30` | Check every N-th frame (lower = more thorough but slower) |
| `--move-threshold PX` | `5` | Minimum pixel shift between samples to count as movement |
| `--min-moves N` | `2` | Consecutive moving detections required to confirm |
| `--verbose` | off | Print per-frame detection details |
| `--help` | — | Show usage information |

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | Moving logo **detected** (`true`) |
| `1` | No moving logo found (`false`) |
| `2` | Error (bad arguments, missing files, etc.) |

## Examples

### Quick check (default settings)

```sh
./src/detect-moving/detect-moving \
  --video ./videos_to_process/my_video.mp4 \
  --config ./video_layouts.json
```

### Catch brief transitions (smaller sample interval)

Logo transitions often last only ~1 second (~30 frames at 30fps). The default `--sample-interval 30` might only land one sample during the slide. Use a smaller interval to catch them reliably:

```sh
./src/detect-moving/detect-moving \
  --video ./videos_to_process/my_video.mp4 \
  --config ./video_layouts.json \
  --sample-interval 10
```

### Single-move confirmation (fastest exit)

If a single large position shift is enough evidence:

```sh
./src/detect-moving/detect-moving \
  --video ./videos_to_process/my_video.mp4 \
  --config ./video_layouts.json \
  --min-moves 1
```

### Verbose output (debugging)

```sh
./src/detect-moving/detect-moving \
  --video ./videos_to_process/my_video.mp4 \
  --config ./video_layouts.json \
  --verbose
```

Sample verbose output:

```
Video : ./videos_to_process/my_video.mp4
  Res : 1920x1080  Frames: 3709  FPS: 30.0343
  Layout: NxtWave Videos (1920x1080)
  Templates loaded: 10
  Sample interval : 30 frames
  Move threshold  : 5 px
  Min moves       : 2

  [Logo Bottom Right] frame 480 (15.9s) pos=(1534,917) first
  [Logo Bottom Right] frame 510 (16.9s) pos=(1534,917) static shift=0px
  ...
  [Logo Bottom Right] frame 2700 (89.9s) pos=(910,917) moved 624px streak=1
```

### Use in a shell script

```sh
if ./src/detect-moving/detect-moving --video "$VIDEO" --config ./video_layouts.json --min-moves 1 2>/dev/null; then
  echo "Video has moving logos — use --track-moving flag with batch-delogo"
else
  echo "No moving logos — standard processing is fine"
fi
```

## How it works

1. Opens the video and auto-detects the layout from `video_layouts.json` based on resolution.
2. Loads all detection templates (reference images) defined in the layout.
3. Samples frames at the configured interval.
4. For each frame, runs OpenCV `TM_CCOEFF_NORMED` template matching for every detection template within its configured search region.
5. Tracks the detected position of each template across consecutive samples.
6. If a template's position shifts by more than `--move-threshold` pixels for `--min-moves` consecutive samples → **moving logo confirmed** → prints `true` and exits with code `0`.
7. If the entire video is scanned without confirming movement → prints `false` and exits with code `1`.

## Performance tips

- **Reduce templates**: Each template is matched on every sampled frame. If you only care about a specific logo, create a dedicated config with just that detection.
- **Increase sample interval**: `--sample-interval 60` halves the work but may miss very brief transitions.
- **Use search quadrants**: Ensure all detections in `video_layouts.json` have `search_quadrant` set (1–4) rather than searching the full frame. Full-frame searches are significantly slower.

