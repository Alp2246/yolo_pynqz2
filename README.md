# Tiny YOLO on PYNQ-Z2 (DPU)

PYNQ-Z2 uzerinde **Tiny YOLO** nesne tespiti: USB webcam ile canli inference, DPU (DNNDK) hizlandirmali.

Bu depo, [YOLO on PYNQ-Z2](https://andre-araujo.gitbook.io/yolo-on-pynq-z2/) kurulumunun uzerine **optimize edilmis** `tiny_yolo_video.cpp` ve Makefile'lari icerir (MJPEG kamera, hizli on-isleme, daha yuksek FPS).

## Gereksinimler

| Parca | Aciklama |
|--------|----------|
| Kart | PYNQ-Z2 |
| SD imaj | PetaLinux **pynqz2_dpu** (DPU + DNNDK) |
| Guc | Regulator (REG jumper) onerilir |
| Kamera | UVC uyumlu USB webcam |
| Host | MobaXterm / X11 (canli pencere icin) veya sadece terminal FPS |

> Model dosyalari (`model/dpu_tiny_yolo.elf`) SD imajinda `~/tiny_yolo_pynqz2/model/` altinda gelir; bu repoya dahil degildir.

## Web panosu (onerilen — X11 gerekmez)

Kameradan canli tespit + kutular tarayicida:

```bash
# Kart (seri): ifconfig eth0 192.168.2.99
# PC (PowerShell, yonetici):
powershell -ExecutionPolicy Bypass -File setup_pc_network.ps1

# Kartta:
cd ~/tiny_yolo_pynqz2
bash run_yolo_web.sh
```

Tarayici: **http://192.168.2.99:8080** — canli goruntu, FPS, tespit listesi.

## Terminal / X11 (klasik)

```bash
cd ~/tiny_yolo_pynqz2
make -f makefile_video clean && make -f makefile_video
./tiny_yolo
```

MobaXterm X11 acikken pencere; terminalde `FPS: 12.x`.

## SD karta DPU imaji yazma (Windows)

1. `pynqz2_dpu_ml16.rar` icinden `.img` cikar (7-Zip).
2. SD karti USB okuyucu ile tak.
3. [balenaEtcher](https://etcher.balena.io/) veya bu repodaki `flash_dpu.ps1` ile **yalnizca SD diske** yaz.
4. Boot jumper **SD**, guc **REG**, DPU kartini tak, ac.

Seri (PuTTY, 115200): kullanici **`root`**, sifre **`root`**.

## Baglanti

| Baglanti | Deger |
|----------|--------|
| Seri | COM port, 115200 baud |
| Ethernet kart | `192.168.2.99` |
| Ethernet PC | `192.168.2.10` / mask `255.255.255.0` |
| SSH | `ssh root@192.168.2.99` |

## Kamera kontrolu

```bash
ls /dev/video0
v4l2-ctl --list-formats-ext -d /dev/video0
```

MJPEG 640x480 destekleniyorsa kod otomatik MJPEG kullanir.

## Optimizasyonlar (`programs/tiny_yolo_video.cpp`)

- Hizli letterbox + quantize (`set_input_image_fast`)
- Kamera: MJPEG, dusuk buffer
- Tek dongu (bellek / stabilite); istege bagli pipeline kodu dosyada yorumlu
- Terminalde sade FPS ciktisi

## Proje yapisi

```
yolo_pynqz2/
├── programs/tiny_yolo_video.cpp   # optimize canli video (X11)
├── programs/tiny_yolo_web.cpp     # web panosu (tarayici)
├── makefile_video / makefile_web
├── run_yolo_web.sh                # derle + web baslat
├── setup_pc_network.ps1           # PC IP 192.168.2.10
├── KOMUTLAR.txt
└── README.md
```

## Tek resim (opsiyonel)

Upstream `tiny_yolo_image.cpp` dosyasini `programs/` altina koyun:

```bash
make -f makefile_image
./tiny_yolo_image dog.jpg
```

## Kaynak / tesekkur

- Temel kurulum ve model: [andre1araujo/YOLO-on-PYNQ-Z2](https://github.com/andre1araujo/YOLO-on-PYNQ-Z2)
- Kitap: [YOLO on PYNQ-Z2](https://andre-araujo.gitbook.io/yolo-on-pynq-z2/)

## Lisans

MIT — bkz. [LICENSE](LICENSE).
