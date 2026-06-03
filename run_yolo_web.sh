#!/bin/bash
# Web panosu: bash run_yolo_web.sh
set -e
cd "$(dirname "$0")"
PW="${PYNQ_PW:-root}"

echo "[1/3] Eski sunucu durduruluyor..."
fuser -k 8080/tcp 2>/dev/null || true
sleep 1

echo "[2/3] Derleme (makefile_web)..."
mkdir -p objects programs
make -f makefile_web clean
make -f makefile_web 2>&1 | tail -6

if [ ! -x ./tiny_yolo_web ]; then
  echo "[HATA] tiny_yolo_web yok"
  exit 1
fi

echo "[3/3] Web sunucusu + YOLO basliyor..."
echo "      Tarayici: http://192.168.2.99:8080"
echo "      USB webcam takili olmali"
exec ./tiny_yolo_web
