#!/bin/sh

# Hizli guncelleme: curl -s http://192.168.2.10:8000/update.sh | sh

PC=http://192.168.2.10:8000

DIR=/root/tiny_yolo_pynqz2

date -s "2026-06-04 01:45:00" 2>/dev/null || true

cd "$DIR"

echo "[1] Yeni kod indiriliyor..."

wget -q "$PC/programs/tiny_yolo_web.cpp" -O programs/tiny_yolo_web.cpp

wget -q "$PC/makefile_web" -O makefile_web

touch programs/tiny_yolo_web.cpp makefile_web

echo "[2] Eski sunucu durduruluyor..."

fuser -k 8080/tcp 2>/dev/null || true

pkill -f tiny_yolo_web 2>/dev/null || true

sleep 1

echo "[3] Derleniyor (1-2 dk)..."

make -f makefile_web 2>&1 | tail -4

if [ ! -x ./tiny_yolo_web ]; then

  echo "HATA: derleme basarisiz"

  exit 1

fi

echo "[4] Baslatiliyor..."

nohup ./tiny_yolo_web >/tmp/yolo_web.log 2>&1 &

sleep 3

if curl -s -m 2 http://127.0.0.1:8080/data | grep -q fps; then

  echo "TAMAM -> http://192.168.2.99:8080 (yenile)"

else

  echo "UYARI: yanit yok"; tail -6 /tmp/yolo_web.log

fi

