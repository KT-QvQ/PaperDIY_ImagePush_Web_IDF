# PaperDIY Image Push Web Demo - ESP-IDF 5.5.4

Project repository: [KT-QvQ/PaperDIY_ImagePush_Web_IDF](https://github.com/KT-QvQ/PaperDIY_ImagePush_Web_IDF)

This ESP-IDF demo pushes PNG and JPG images from a phone or computer to the
PaperDIY display over a Wi-Fi access point. It uses the native ESP-IDF
Wi-Fi AP and `esp_http_server` components. M5GFX/M5Unified are reused for the
PaperDIY display driver.

For the prebuilt Factory firmware and end-user instructions, see
[`USER_README.md`](USER_README.md).

## Dependency

M5Unified `0.2.26` and its M5GFX dependency are declared in
`main/idf_component.yml`. ESP-IDF's component manager downloads them from the
public component registry during the first configure/build.

## Build

```bash
# Run this after activating your ESP-IDF 5.5 installation.
cd PaperDIY_ImagePush_Web_IDF
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
