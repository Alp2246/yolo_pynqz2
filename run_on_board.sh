#!/bin/bash
# Kartta: bash run_on_board.sh
set -e
cd "$(dirname "$0")"
mkdir -p objects programs

echo "[1/3] Derleme..."
make -f makefile_video clean
make -f makefile_video 2>&1 | tail -8

echo "[2/3] Binary adi (makefile -> tiny_yolo)..."
if [ -x ./tiny_yolo ]; then
  ln -sf tiny_yolo tiny_yolo_video 2>/dev/null || true
fi

echo "[3/3] Calistir (q ile cik, X11 gerekir)..."
echo "      Sadece FPS icin MobaXterm X11 acik olmali."
exec ./tiny_yolo
