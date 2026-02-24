# multi-delogo

Batch video processing tool that automatically detects and replaces logos using template matching.


## Features

- Template matching to detect logos at predefined positions
- Replace detected logos with custom images
- Process multiple videos in one command
- Configurable detection rules per video layout


## Project Structure

```
multi-delogo/
├── src/batch-delogo/           # CLI batch processing tool
├── reference_images/           # Reference images for logo detection
├── overlay-images/             # Replacement images for logo overlay
├── videos_to_process/          # Input videos
├── processed_videos/           # Output videos
└── video_layouts.json          # Detection and replacement rules
```


## Installation

### 1. Install build tools (Debian/Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y autoconf automake autopoint gettext libtool pkg-config
```

### 2. Install library dependencies (Debian/Ubuntu)

```sh
sudo apt-get install -y libglib2.0-dev libgtkmm-3.0-dev libgoocanvas-2.0-dev \
  libopencv-dev libjsoncpp-dev libboost-all-dev
```

### 3. Clone and build

```sh
git clone https://github.com/Lakshmi-Narayana-K/multi-delogo.git
cd multi-delogo
git checkout lakshmi-changes
./autogen.sh
./configure
make
```


## Configuration

### Video Layouts (`video_layouts.json`)

Define detection rules for your videos:

```json
{
  "layouts": [
    {
      "name": "my_layout",
      "resolution": { "width": 1920, "height": 1080 },
      "detections": [
        {
          "name": "Top Right Logo",
          "reference_image": "reference_images/logo.png",
          "search_region": { "x": 1500, "y": 0, "width": 420, "height": 150 },
          "match_threshold": 0.6,
          "replacement": {
            "image_path": "overlay-images/replacement.png",
            "position": { "x": 1580, "y": 50 },
            "size": { "width": 300, "height": 80 },
            "scale": 1.0
          }
        }
      ]
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `reference_image` | Image to search for (the logo to detect) |
| `search_region` | Area of the frame to search within (x, y, width, height in pixels) |
| `search_quadrant` | Alternative to `search_region`: 1=top-left, 2=top-right, 3=bottom-right, 4=bottom-left (searches that quarter of the frame) |
| `match_threshold` | Detection sensitivity (0.0-1.0, lower = more lenient) |
| `replacement` | Image and position to overlay when logo is found |


## Usage

### Basic Command

```sh
./src/batch-delogo/batch-delogo \
  --input-folder ./videos_to_process \
  --output-folder ./processed_videos \
  --config ./video_layouts.json \
  --auto-detect
```

### Test only the first 1 minute (faster iteration)

```sh
# 1) Create a 1-minute clip (no re-encode)
ffmpeg -i "videos-standby/Abhinav- 30 mins format.mp4" -t 60 -c copy "videos_to_process/Abhinav_1min.mp4"

# 2) Run batch-delogo on the clip
./src/batch-delogo/batch-delogo \
  --input-folder ./videos_to_process \
  --output-folder ./processed_videos \
  --config ./video_layouts.json \
  --auto-detect
```

### CLI Options

| Option | Description |
|--------|-------------|
| `--input-folder` | Folder containing videos to process |
| `--output-folder` | Folder for processed videos |
| `--config` | Path to video_layouts.json |
| `--layout` | Specific layout name (optional) |
| `--auto-detect` | Enable template matching detection |
| `--dry-run` | Preview without processing |


## Workflow

1. **Create reference images** - Crop logos from video frames
   ```sh
   ffmpeg -i video.mp4 -vf "select=eq(n\,100)" -vframes 1 frame.png
   # Then crop the logo area and save to reference_images/
   ```

2. **Add replacement images** - Place in `overlay-images/`

3. **Configure detection rules** - Edit `video_layouts.json`

4. **Test with dry-run**
   ```sh
   ./src/batch-delogo/batch-delogo --input-folder ./videos_to_process \
     --output-folder ./processed_videos --config ./video_layouts.json \
     --auto-detect --dry-run
   ```

5. **Process videos**
   ```sh
   ./src/batch-delogo/batch-delogo --input-folder ./videos_to_process \
     --output-folder ./processed_videos --config ./video_layouts.json \
     --auto-detect
   ```


## Copyright

multi-delogo is Copyright (C) 2018-2025 Werner Turing <werner.turing@protonmail.com>

Licensed under GNU General Public License v3.0.

