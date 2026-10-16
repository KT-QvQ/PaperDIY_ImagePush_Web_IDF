# PaperDIY Image Push Web Demo - ESP-IDF 5.5.4

This ESP-IDF demo pushes PNG and JPG images from a phone or computer to the
PaperDIY display over a Wi-Fi access point. It uses the native ESP-IDF
Wi-Fi AP and `esp_http_server` components. M5GFX/M5Unified are reused for the
PaperDIY display driver.

## Dependency

The component wrappers reference the local M5 libraries at:

`/home/kt/Paper_Mono/PaperMonoWakeDemo/lib/M5GFX`

`/home/kt/Paper_Mono/PaperMonoWakeDemo/lib/M5Unified`

## Build

```bash
cd /home/kt/PaperDIY/PaperDIY_ImagePush_Web_IDF
source /home/kt/idf554/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

Flash and monitor with the PaperDIY serial port:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

The exact device path may differ.

## Use

Connect to the AP `PaperDIY-PNG` with password `paperdiy`, then open
`http://192.168.4.1/`. The page sends PNG or JPG files directly to
`POST /api/push`. The server checks the file signature, scales the image to
fit the display, and supports files up to 2.4 MiB.

PNG command-line example:

```bash
curl -X POST -H 'Content-Type: image/png' \
  --data-binary @your.png http://192.168.4.1/api/push
```

JPG command-line example:

```bash
curl -X POST -H 'Content-Type: image/jpeg' \
  --data-binary @your.jpg http://192.168.4.1/api/push
```
