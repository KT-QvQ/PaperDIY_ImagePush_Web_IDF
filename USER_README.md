# PaperDIY Image Push Web Firmware

这是 PaperDIY 的 Factory 固件使用说明。烧录后，PaperDIY 会创建一个 Wi-Fi
热点，手机或电脑连接后即可通过网页把 PNG/JPG 图片推送到墨水屏显示。

## 固件包

Factory 合并固件位于：

`factory/PaperDIY_ImagePush_Web_IDF_factory.bin`

该文件已经包含 Bootloader、分区表和应用固件，烧录地址为 `0x0`。

## 烧录

使用 ESP32-S3 的 USB 串口，将 `PORT` 替换为实际串口名称：

```bash
python -m esptool --chip esp32s3 --port PORT --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 factory/PaperDIY_ImagePush_Web_IDF_factory.bin
```

如果需要清除旧图片和旧配置，先执行一次全片擦除，再烧录 Factory 固件：

```bash
python -m esptool --chip esp32s3 --port PORT erase_flash
```

擦除操作会删除设备上的旧图片。

## 推送图片

1. 给 PaperDIY 上电，等待设备启动。
2. 手机或电脑连接 Wi-Fi：`PaperDIY-PNG`
3. Wi-Fi 密码：`paperdiy`
4. 浏览器打开：<http://192.168.4.1/>
5. 选择图片，确认预览后点击 **Upload**。

网页会显示上传进度和当前屏幕状态。上传成功后，图片会自动刷新到墨水屏。

## 图片要求

- 支持 PNG 和 JPG。
- 单个文件最大 `2.4 MiB`。
- 图片宽高最大 `4096` 像素。
- JPG 使用 Baseline Sequential 格式；Progressive JPG 不支持。
- 图片会根据横竖比例自动选择方向，等比例缩放并居中显示。
- 设备会保留最后一次成功上传的图片，重启后会再次显示。

网页会对较大的图片进行客户端压缩，以减少传输时间；原文件不会被修改。

## 常见问题

### 找不到热点

确认设备已经完成启动，并重新断电上电。通过串口监视器查看是否出现
`AP ready: SSID=PaperDIY-PNG`。

### 页面打不开

确认手机仍连接 `PaperDIY-PNG`，并关闭移动数据或 VPN 后重新访问
`http://192.168.4.1/`。

### 上传失败

检查文件格式和大小，Progressive JPG 请转换为普通 JPG；如果提示 SPIFFS 空间不足，
可先执行全片擦除后重新烧录 Factory 固件。

### 图片显示较慢

墨水屏刷新和大图片解码需要时间。优先使用接近屏幕比例、尺寸适中的图片，网页会先
压缩较大的图片再上传。

## 命令行上传

也可以直接使用 HTTP API：

```bash
curl -X POST -H 'Content-Type: image/png' \
  --data-binary @image.png http://192.168.4.1/api/push
```

查询当前图片状态：

```bash
curl http://192.168.4.1/api/status
```
