#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

clear
echo "🎬 Converting MJPEG → MP4"
echo "📁 Location: $SCRIPT_DIR"
echo ""

RAW_FOLDER=".raw"
shopt -s nullglob
files=("$RAW_FOLDER"/*.mjpeg)
total=${#files[@]}

if [ "$total" -eq 0 ]; then
    echo "ℹ️ No video files found"
    read -p "Press [Return] to exit..."
    exit 0
fi

count=0
bar_width=20

for file in "${files[@]}"; do
    base=$(basename "$file" .mjpeg)
    output="${base}.mp4"

    # Convert (silent)
    ffmpeg -y -framerate 24 -i "$file" \
        -vf "transpose=2" \
        -vcodec libx264 -pix_fmt yuv420p \
        -profile:v high -level:v 4.0 -movflags +faststart \
        "$output" > /dev/null 2>&1

    if [ -f "$output" ]; then
        rm "$file"
    fi

    count=$((count + 1))
    percent=$((100 * count / total))
    filled=$((bar_width * count / total))
    empty=$((bar_width - filled))
    bar=$(printf "%${filled}s" | tr ' ' '█')$(printf "%${empty}s" | tr ' ' '_')

    printf "\r🔄 Progress: [%s] %3d%%" "$bar" "$percent"
done

echo ""
echo "✅ Done – $total videos converted."
read -p "Press [Return] to exit..."
