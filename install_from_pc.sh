#!/bin/sh
# Kart seri konsolda: curl -s http://192.168.2.10:8000/install_from_pc.sh | sh
PC=http://192.168.2.10:8000
DIR=/root/tiny_yolo_pynqz2

echo "[0/7] Saat duzeltiliyor (make icin)..."
date -s "2026-06-04 01:30:00" 2>/dev/null || true

echo "[1/7] SSH..."
mkdir -p /root/.ssh
chmod 700 /root/.ssh
grep -q 'yolo' /root/.ssh/authorized_keys 2>/dev/null || \
  echo 'ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDF/DLfmNnXQOE+m9Plk3W+PhGcH0g/cNTLuVm47DuCxyKuUQIu1DrTj3HUUkR4v8Am0Sc0tXj/zzhTlJSOTfOkTJ208V/DEeW+qb5/+Btb1hCPpz0gZPvff5HrS4dHakGSmgouX3YUaDbbvBcVLX2z/wa1m6NaeIbhap7n1m78AJTkcDWAYYv38mfTD72r77tmdNGr7DaaTkka5bO/cDg+XRcBT9Cgaz3zMvvcsPYyR8KGyep2KjqXNWXf6QP+Rgc1R9LlUaKB7k6Zt3gM1CA8Y04Hifq1DfoOSvy+KS2AH7RoYBDdLLOBJ64C5vPs1NuioY7kVuwr5ekeiflcNbFf yolo@pynq' >> /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
echo 'root:root' | chpasswd 2>/dev/null || true
if [ -f /etc/ssh/sshd_config ]; then
  sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config
  sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
fi
/etc/init.d/sshd restart 2>/dev/null || killall -HUP sshd 2>/dev/null || true

echo "[2/7] DNNDK..."
if [ ! -f /usr/lib/libn2cube.so ] && [ ! -f /usr/lib/libn2cube.so.1 ]; then
  if [ -d /root/zynq7020_dnndk_v3.1 ]; then
    cd /root/zynq7020_dnndk_v3.1 && ./install.sh 2>&1 | tail -3
  else
    echo "  UYARI: DNNDK kurulum klasoru yok"
  fi
fi

echo "[3/7] Model..."
mkdir -p "$DIR/programs" "$DIR/objects" "$DIR/model"
cd "$DIR"

if [ ! -f model/dpu_tiny_yolo.elf ]; then
  echo "  PC'den indiriliyor..."
  wget -q "$PC/model/dpu_tiny_yolo.elf" -O model/dpu_tiny_yolo.elf || \
  wget -q "https://github.com/andre1araujo/YOLO-on-PYNQ-Z2/raw/main/Deployment/tiny_yolo_pynqz2/model/dpu_tiny_yolo.elf" -O model/dpu_tiny_yolo.elf
fi
if [ -f model/dpu_tiny_yolo.elf ]; then
  echo "  Model OK: $(ls -lh model/dpu_tiny_yolo.elf | awk '{print $5}')"
else
  echo "  HATA: model indirilemedi!"
  exit 1
fi

echo "  DNNDK kutuphane..."
if ldconfig -p 2>/dev/null | grep -q n2cube || ls /usr/lib/libn2cube* >/dev/null 2>&1; then
  echo "  libn2cube OK"
else
  echo "  libn2cube YOK - araniyor..."
  find / -name 'libn2cube*' 2>/dev/null | head -3
  if [ -d /root/zynq7020_dnndk_v3.1 ]; then
    echo "  DNNDK kuruluyor..."
    cd /root/zynq7020_dnndk_v3.1 && ./install.sh 2>&1 | tail -2
    cd "$DIR"
  else
    echo "  UYARI: DNNDK lib yok ve kurulum klasoru yok!"
    echo "  Imajda DNNDK gomulu degilse derleme basarisiz olur."
  fi
fi

echo "[4/7] Dosyalar indiriliyor..."
wget -q "$PC/programs/tiny_yolo_web.cpp" -O programs/tiny_yolo_web.cpp
wget -q "$PC/makefile_web" -O makefile_web
wget -q "$PC/run_yolo_web.sh" -O run_yolo_web.sh
chmod +x run_yolo_web.sh
touch programs/tiny_yolo_web.cpp makefile_web

echo "[5/7] Kamera..."
ls -la /dev/video0 2>/dev/null || echo "  UYARI: webcam tak"

echo "[6/7] Web panosu derleniyor..."
make -f makefile_web clean 2>/dev/null || true
if make -f makefile_web 2>&1; then
  echo "  Derleme OK"
else
  echo ""
  echo "  HATA: web derlemesi basarisiz."
  echo "  Yedek test: orijinal dog.jpg ile DPU dogrulamasi..."
  wget -q "$PC/upstream/programs/tiny_yolo_image.cpp" -O programs/tiny_yolo_image.cpp
  wget -q "$PC/upstream/makefile" -O makefile_orig
  wget -q "$PC/upstream/dog.jpg" -O dog.jpg
  if make -f makefile_orig 2>&1 | tail -5; then
    echo "  -> Orijinal derlendi. Test: ./tiny_yolo_image dog.jpg"
    echo "  Demek ki DPU+lib calisiyor; web kodunda sorun var (bana log yolla)."
  else
    echo "  -> Orijinal da derlenmedi: DNNDK kutuphane eksik."
    echo "  ldconfig -p | grep -E 'n2cube|hineon|dputils' ciktisini yolla."
  fi
  exit 1
fi

echo "[7/7] Web baslatiliyor..."
fuser -k 8080/tcp 2>/dev/null || true
sleep 1
# GPS cihazi otomatik algilanir (USB-TTL takiliysa); yoksa GPS kapali calisir.
GPS_ARG="$GPS_DEV"
if [ -z "$GPS_ARG" ]; then
  for d in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyPS1; do
    if [ -e "$d" ]; then GPS_ARG="$d"; break; fi
  done
fi
if [ -n "$GPS_ARG" ]; then
  echo "  GPS: $GPS_ARG"
  nohup ./tiny_yolo_web "$GPS_ARG" >/tmp/yolo_web.log 2>&1 &
else
  nohup ./tiny_yolo_web >/tmp/yolo_web.log 2>&1 &
fi
sleep 3
if curl -s -m 2 http://127.0.0.1:8080/data | grep -q fps; then
  echo ""
  echo "=========================================="
  echo "  TAMAM -> http://192.168.2.99:8080"
  echo "  Log: tail -f /tmp/yolo_web.log"
  echo "=========================================="
else
  echo "  UYARI: sunucu henuz yanit vermedi"
  tail -8 /tmp/yolo_web.log 2>/dev/null
fi
