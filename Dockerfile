# ── Stage 1: Build the C++ binary ────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    libopencv-dev \
    libjsoncpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy only what's needed to compile batch-delogo
COPY src/filter-generator/ ./src/filter-generator/
COPY src/batch-delogo/batch-delogo.cpp ./src/batch-delogo/

# Compile filter-generator static library
RUN cd src/filter-generator && \
    CPPFLAGS=$(pkg-config --cflags jsoncpp) && \
    g++ -std=c++17 -O2 $CPPFLAGS -c \
        FilterData.cpp FilterFactory.cpp FilterList.cpp Filters.cpp \
        ImagePreset.cpp IOUtils.cpp RegularScriptGenerator.cpp \
        ScriptGenerator.cpp VideoLayoutConfig.cpp && \
    ar rcs libfilter-generator.a *.o

# Compile batch-delogo binary
RUN g++ -std=c++17 -O2 \
    $(pkg-config --cflags opencv4 jsoncpp) \
    -I./src \
    src/batch-delogo/batch-delogo.cpp \
    src/filter-generator/libfilter-generator.a \
    $(pkg-config --libs opencv4 jsoncpp) \
    -lopencv_imgcodecs \
    -o batch-delogo


# ── Stage 2: Runtime image ────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    libopencv-videoio406t64 \
    libopencv-imgproc406t64 \
    libopencv-core406t64 \
    libopencv-imgcodecs406t64 \
    libjsoncpp25 \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir --break-system-packages boto3

WORKDIR /app

# Binary from builder
COPY --from=builder /build/batch-delogo ./batch-delogo

# Config and assets baked into image
COPY video_layouts.json     ./video_layouts.json
COPY reference_images/      ./reference_images/
COPY overlay-images/        ./overlay-images/

# Working folders the script uses
RUN mkdir -p videos_to_process processed_videos

COPY pipeline.py ./pipeline.py

CMD ["python3", "pipeline.py"]
