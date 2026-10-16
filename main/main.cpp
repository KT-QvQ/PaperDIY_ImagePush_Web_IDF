#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "M5Unified.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "PaperDIYImagePushIDF";
constexpr char kApSsid[] = "PaperDIY-PNG";
constexpr char kApPassword[] = "paperdiy";
constexpr char kFsBase[] = "/spiffs";
constexpr char kImagePath[] = "/spiffs/upload.png";
constexpr char kTempPath[] = "/spiffs/upload.tmp";
constexpr size_t kMaxImageBytes = 2400U * 1024U;
constexpr uint32_t kMaxImageDimension = 4096;
constexpr size_t kReceiveBufferSize = 65536;

enum class ImageFormat : uint8_t {
  kPng,
  kJpeg,
};

struct ImageInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  size_t bytes = 0;
  ImageFormat format = ImageFormat::kPng;
};

struct UploadState {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t bytes = 0;
  ImageInfo info;
  const char* error = nullptr;
  bool failed = false;
  bool accepted = false;
};

httpd_handle_t s_http = nullptr;

struct DisplayJob {
  ImageInfo info;
  TaskHandle_t waiter = nullptr;
  bool success = false;
};

TaskHandle_t s_display_task = nullptr;
SemaphoreHandle_t s_display_gate = nullptr;
DisplayJob s_display_job;
uint8_t s_decode_yield_rows = 0;

uint32_t read_be32(const uint8_t* value) {
  return (static_cast<uint32_t>(value[0]) << 24) |
         (static_cast<uint32_t>(value[1]) << 16) |
         (static_cast<uint32_t>(value[2]) << 8) |
         static_cast<uint32_t>(value[3]);
}

uint16_t read_be16(const uint8_t* value) {
  return static_cast<uint16_t>((static_cast<uint16_t>(value[0]) << 8) |
                               static_cast<uint16_t>(value[1]));
}

const char* format_name(ImageFormat format) {
  return format == ImageFormat::kJpeg ? "JPEG" : "PNG";
}

bool inspect_png(const char* path, ImageInfo* info, const char** error) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    if (error != nullptr) *error = "file open failed";
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    if (error != nullptr) *error = "file seek failed";
    return false;
  }
  const long file_size = std::ftell(file);
  if (file_size < 33 || static_cast<size_t>(file_size) > kMaxImageBytes) {
    std::fclose(file);
    if (error != nullptr) *error = "PNG size is outside the supported range";
    return false;
  }
  std::rewind(file);

  uint8_t header[24] = {};
  if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
    std::fclose(file);
    if (error != nullptr) *error = "PNG header is incomplete";
    return false;
  }
  std::fclose(file);

  static constexpr uint8_t kPngSignature[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  if (std::memcmp(header, kPngSignature, sizeof(kPngSignature)) != 0 ||
      read_be32(header + 8) != 13 || header[12] != 'I' ||
      header[13] != 'H' || header[14] != 'D' || header[15] != 'R') {
    if (error != nullptr) *error = "file is not a PNG or IHDR is missing";
    return false;
  }

  const uint32_t width = read_be32(header + 16);
  const uint32_t height = read_be32(header + 20);
  if (width == 0 || height == 0 || width > kMaxImageDimension ||
      height > kMaxImageDimension) {
    if (error != nullptr) *error = "PNG dimensions are outside the supported range";
    return false;
  }

  if (info != nullptr) {
    info->width = width;
    info->height = height;
    info->bytes = static_cast<size_t>(file_size);
    info->format = ImageFormat::kPng;
  }
  return true;
}

bool inspect_jpeg(const char* path, ImageInfo* info, const char** error) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    if (error != nullptr) *error = "file open failed";
    return false;
  }

  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    if (error != nullptr) *error = "file seek failed";
    return false;
  }
  const long file_size = std::ftell(file);
  if (file_size < 4 || static_cast<size_t>(file_size) > kMaxImageBytes) {
    std::fclose(file);
    if (error != nullptr) *error = "JPEG size is outside the supported range";
    return false;
  }
  std::rewind(file);

  uint8_t soi[2] = {};
  if (std::fread(soi, 1, sizeof(soi), file) != sizeof(soi) || soi[0] != 0xff ||
      soi[1] != 0xd8) {
    std::fclose(file);
    if (error != nullptr) *error = "file is not a JPEG";
    return false;
  }

  long offset = 2;
  uint32_t width = 0;
  uint32_t height = 0;
  bool found_frame = false;
  while (offset + 1 < file_size) {
    if (std::fseek(file, offset, SEEK_SET) != 0) break;
    int marker_prefix = std::fgetc(file);
    if (marker_prefix != 0xff) {
      ++offset;
      continue;
    }
    int marker = std::fgetc(file);
    while (marker == 0xff) marker = std::fgetc(file);
    if (marker == EOF) break;
    const long segment_offset = std::ftell(file);
    if (segment_offset < 0) break;
    offset = segment_offset;

    // Standalone markers do not carry a segment length.
    if (marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
        (marker >= 0xd0 && marker <= 0xd7)) {
      if (marker == 0xd9) break;
      continue;
    }
    if (offset + 2 > file_size || std::fseek(file, offset, SEEK_SET) != 0) break;
    uint8_t length_bytes[2] = {};
    if (std::fread(length_bytes, 1, sizeof(length_bytes), file) != sizeof(length_bytes)) {
      break;
    }
    const uint16_t segment_length = read_be16(length_bytes);
    if (segment_length < 2 ||
        offset + static_cast<long>(segment_length) > file_size) {
      break;
    }

    // M5GFX's built-in decoder supports baseline sequential JPEG (SOF0).
    // Progressive and extended JPEGs are rejected before replacing the image.
    const bool is_frame = marker == 0xc0;
    if (is_frame) {
      if (segment_length < 8 || std::fseek(file, offset + 2, SEEK_SET) != 0) break;
      uint8_t frame_header[6] = {};
      if (std::fread(frame_header, 1, sizeof(frame_header), file) != sizeof(frame_header)) {
        break;
      }
      height = read_be16(frame_header + 1);
      width = read_be16(frame_header + 3);
      const uint8_t components = frame_header[5];
      if ((components != 1 && components != 3) ||
          segment_length < static_cast<uint16_t>(8 + 3 * components)) {
        break;
      }
      found_frame = true;
      break;
    }
    offset += segment_length;
    if (marker == 0xda) break;
  }
  std::fclose(file);

  if (!found_frame || width == 0 || height == 0 || width > kMaxImageDimension ||
      height > kMaxImageDimension) {
    if (error != nullptr) *error = "JPEG frame or dimensions are invalid";
    return false;
  }
  if (info != nullptr) {
    info->width = width;
    info->height = height;
    info->bytes = static_cast<size_t>(file_size);
    info->format = ImageFormat::kJpeg;
  }
  return true;
}

bool inspect_image(const char* path, ImageInfo* info, const char** error) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    if (error != nullptr) *error = "file open failed";
    return false;
  }
  uint8_t signature[8] = {};
  const size_t read = std::fread(signature, 1, sizeof(signature), file);
  std::fclose(file);

  static constexpr uint8_t kPngSignature[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  if (read >= sizeof(kPngSignature) &&
      std::memcmp(signature, kPngSignature, sizeof(kPngSignature)) == 0) {
    return inspect_png(path, info, error);
  }
  if (read >= 2 && signature[0] == 0xff && signature[1] == 0xd8) {
    return inspect_jpeg(path, info, error);
  }
  if (error != nullptr) *error = "file is not a supported PNG or JPEG";
  return false;
}

class PosixFileWrapper final : public lgfx::DataWrapper {
 public:
  ~PosixFileWrapper() override { close(); }

  bool open(const char* path) override {
    close();
    file_ = std::fopen(path, "rb");
    return file_ != nullptr;
  }

  int read(uint8_t* buffer, uint32_t length) override {
    return file_ == nullptr ? 0 : static_cast<int>(std::fread(buffer, 1, length, file_));
  }

  void skip(int32_t offset) override {
    if (file_ != nullptr) std::fseek(file_, offset, SEEK_CUR);
  }

  bool seek(uint32_t offset) override {
    return file_ != nullptr && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
  }

  void close() override {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  int32_t tell() override {
    return file_ == nullptr ? -1 : static_cast<int32_t>(std::ftell(file_));
  }

 private:
  FILE* file_ = nullptr;
};

bool display_image(const char* path, const ImageInfo& info) {
  const int64_t decode_started = esp_timer_get_time();
  const uint8_t rotation = info.width >= info.height ? 1 : 0;
  M5.Display.setRotation(rotation);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);

  const int32_t display_width = M5.Display.width();
  const int32_t display_height = M5.Display.height();
  const float fit_scale = std::min(
      static_cast<float>(display_width) / static_cast<float>(info.width),
      static_cast<float>(display_height) / static_cast<float>(info.height));
  const float scale = std::min(1.0f, fit_scale);
  const int32_t image_width = std::max<int32_t>(
      1, static_cast<int32_t>(info.width * scale + 0.5f));
  const int32_t image_height = std::max<int32_t>(
      1, static_cast<int32_t>(info.height * scale + 0.5f));
  const int32_t x = (display_width - image_width) / 2;
  const int32_t y = (display_height - image_height) / 2;

  M5.Display.fillScreen(TFT_WHITE);
  PosixFileWrapper file;
  s_decode_yield_rows = 0;
  file.fp_post_read = [](lgfx::LGFXBase*) {
    // Keep the decoder responsive without adding a fixed 1 ms delay per row.
    if (++s_decode_yield_rows >= 8) {
      s_decode_yield_rows = 0;
      taskYIELD();
    }
  };
  const bool decoded = file.open(path) &&
      (info.format == ImageFormat::kJpeg
           ? M5.Display.drawJpg(&file, x, y, 0, 0, 0, 0, scale, 0.0f,
                                datum_t::top_left)
           : M5.Display.drawPng(&file, x, y, 0, 0, 0, 0, scale, 0.0f,
                                datum_t::top_left));
  if (!decoded) {
    M5.Display.display();
    file.close();
    ESP_LOGE(kTag, "display: %s decode failed after %lld ms", format_name(info.format),
             static_cast<long long>((esp_timer_get_time() - decode_started) / 1000));
    return false;
  }
  file.close();
  const int64_t decode_finished = esp_timer_get_time();
  M5.Display.display();
  M5.Display.waitDisplay();
  const int64_t refresh_finished = esp_timer_get_time();
  ESP_LOGI(kTag, "display: %s %s (%lux%lu, %u bytes, decode=%lld ms refresh=%lld ms)",
           format_name(info.format),
           path,
           static_cast<unsigned long>(info.width),
           static_cast<unsigned long>(info.height),
           static_cast<unsigned>(info.bytes),
           static_cast<long long>((decode_finished - decode_started) / 1000),
           static_cast<long long>((refresh_finished - decode_finished) / 1000));
  return true;
}

void display_worker_task(void*) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const ImageInfo info = s_display_job.info;
    const TaskHandle_t waiter = s_display_job.waiter;
    s_display_job.success = display_image(kImagePath, info);
    if (waiter != nullptr) xTaskNotifyGive(waiter);
  }
}

void start_display_worker() {
  s_display_gate = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(s_display_gate == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
  const BaseType_t result = xTaskCreatePinnedToCore(
      display_worker_task, "paper_display", 8192, nullptr, 0, &s_display_task, 1);
  ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

bool display_image_async(const ImageInfo& info) {
  if (s_display_task == nullptr || s_display_gate == nullptr) {
    return display_image(kImagePath, info);
  }
  xSemaphoreTake(s_display_gate, portMAX_DELAY);
  s_display_job.info = info;
  s_display_job.success = false;
  s_display_job.waiter = xTaskGetCurrentTaskHandle();
  xTaskNotifyGive(s_display_task);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  const bool success = s_display_job.success;
  s_display_job.waiter = nullptr;
  xSemaphoreGive(s_display_gate);
  return success;
}

void reset_upload(UploadState* state) {
  if (state->data != nullptr) {
    heap_caps_free(state->data);
    state->data = nullptr;
  }
  std::remove(kTempPath);
  *state = UploadState();
}

bool start_upload(UploadState* state, size_t capacity) {
  reset_upload(state);
  state->data = static_cast<uint8_t*>(heap_caps_malloc(
      capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (state->data == nullptr) {
    state->failed = true;
    state->error = "not enough PSRAM for upload";
    return false;
  }
  state->capacity = capacity;
  return true;
}

bool finish_upload(UploadState* state) {
  if (state->failed) {
    if (state->data != nullptr) {
      heap_caps_free(state->data);
      state->data = nullptr;
    }
    std::remove(kTempPath);
    return false;
  }

  // Remove the old image only after the new upload is safely buffered in PSRAM.
  // This keeps a previously displayed image available when allocation or receive fails.
  if (std::remove(kImagePath) != 0 && errno != ENOENT) {
    ESP_LOGW(kTag, "SPIFFS: could not remove old image errno=%d (%s)", errno,
             std::strerror(errno));
  }
  size_t total = 0;
  size_t used = 0;
  if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
    const size_t free_bytes = total > used ? total - used : 0;
    ESP_LOGI(kTag, "SPIFFS before upload: used=%u free=%u required=%u",
             static_cast<unsigned>(used), static_cast<unsigned>(free_bytes),
             static_cast<unsigned>(state->bytes));
    const size_t gc_target = state->bytes + 64U * 1024U;
    if (free_bytes < gc_target) {
      const esp_err_t gc_result = esp_spiffs_gc("spiffs", gc_target);
      ESP_LOGI(kTag, "SPIFFS GC: target=%u result=%s", static_cast<unsigned>(gc_target),
               esp_err_to_name(gc_result));
    }
  }

  const int64_t storage_started = esp_timer_get_time();
  FILE* file = std::fopen(kTempPath, "wb");
  if (file == nullptr) {
    const int open_errno = errno;
    ESP_LOGE(kTag, "SPIFFS: could not create temporary file errno=%d (%s)", open_errno,
             std::strerror(open_errno));
    state->error = open_errno == ENOSPC ? "SPIFFS is full; upload a smaller image" :
                   "could not create temporary SPIFFS file";
    heap_caps_free(state->data);
    state->data = nullptr;
    return false;
  }
  setvbuf(file, nullptr, _IOFBF, 32768);
  const size_t written_bytes = std::fwrite(state->data, 1, state->bytes, file);
  const int close_result = std::fclose(file);
  const int write_errno = errno;
  heap_caps_free(state->data);
  state->data = nullptr;
  if (written_bytes != state->bytes || close_result != 0) {
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
      ESP_LOGE(kTag, "SPIFFS write failed: wrote=%u/%u close=%d errno=%d (%s) used=%u free=%u",
               static_cast<unsigned>(written_bytes), static_cast<unsigned>(state->bytes),
               close_result, write_errno, std::strerror(write_errno),
               static_cast<unsigned>(used),
               static_cast<unsigned>(total > used ? total - used : 0));
    } else {
      ESP_LOGE(kTag, "SPIFFS write failed: wrote=%u/%u close=%d errno=%d (%s)",
               static_cast<unsigned>(written_bytes), static_cast<unsigned>(state->bytes),
               close_result, write_errno, std::strerror(write_errno));
    }
    state->error = write_errno == ENOSPC ? "SPIFFS is full; upload a smaller image" :
                   "SPIFFS write failed";
    std::remove(kTempPath);
    return false;
  }
  ESP_LOGI(kTag, "HTTP storage: wrote %u bytes in %llums",
           static_cast<unsigned>(state->bytes),
           static_cast<unsigned long long>(
               (esp_timer_get_time() - storage_started) / 1000));
  if (!inspect_image(kTempPath, &state->info, &state->error)) {
    std::remove(kTempPath);
    return false;
  }
  if (std::rename(kTempPath, kImagePath) != 0) {
    state->error = "could not commit image to SPIFFS";
    std::remove(kTempPath);
    return false;
  }
  state->accepted = true;
  return true;
}

void tune_upload_socket(httpd_req_t* request) {
  const int socket_fd = httpd_req_to_sockfd(request);
  if (socket_fd < 0) return;

#if CONFIG_LWIP_SO_RCVBUF
  int receive_buffer = 65535;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                 sizeof(receive_buffer)) != 0) {
    ESP_LOGW(kTag, "HTTP socket: could not enlarge receive buffer");
  }
#endif

  int no_delay = 1;
  if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                 sizeof(no_delay)) != 0) {
    ESP_LOGW(kTag, "HTTP socket: could not enable TCP_NODELAY");
  }
}

esp_err_t send_text(httpd_req_t* request, const char* type, const char* text,
                    const char* status = nullptr) {
  httpd_resp_set_type(request, type);
  if (status != nullptr) httpd_resp_set_status(request, status);
  return httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_error(httpd_req_t* request, const char* status, const char* text) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_index(httpd_req_t* request) {
  static constexpr char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#0d1117">
<title>PaperDIY</title><style>
:root{color-scheme:dark;--bg:#0d1117;--panel:#151c25;--panel2:#1b2430;--line:#2b3847;--text:#f4f7fb;--muted:#91a0b2;--accent:#9be3c0;--accent2:#f2b66d;--danger:#f28b82;--shadow:0 20px 55px rgba(0,0,0,.22)}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 15% 0,#1d2b37 0,#0d1117 42%);color:var(--text);font:15px/1.5 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;min-height:100vh}button,input{font:inherit}button{cursor:pointer}
.shell{max-width:1080px;margin:0 auto;padding:34px 22px 48px}.topbar{display:flex;align-items:center;justify-content:space-between;gap:24px;margin-bottom:34px}.brand{display:flex;align-items:center;gap:13px}.brand-mark{width:42px;height:42px;border:1px solid #6ec5a0;border-radius:12px;display:grid;place-items:center;color:#0d1714;background:var(--accent);font-weight:900;font-size:20px;box-shadow:0 8px 24px rgba(155,227,192,.18)}.brand h1{font-size:20px;line-height:1.1;margin:0;letter-spacing:0}.brand p{color:var(--muted);font-size:12px;margin:5px 0 0}.online{display:inline-flex;align-items:center;gap:8px;border:1px solid #315541;border-radius:99px;padding:7px 11px;color:#b8ebcf;font-size:12px;background:#14231d;white-space:nowrap}.online i{display:block;width:7px;height:7px;background:var(--accent);border-radius:50%;box-shadow:0 0 0 4px rgba(155,227,192,.12)}
.intro{margin-bottom:26px}.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:0;color:var(--accent2);font-weight:700}.intro h2{font-size:40px;letter-spacing:0;line-height:1.04;margin:9px 0 9px;max-width:640px}.intro p{color:var(--muted);margin:0;max-width:650px}.layout{display:grid;grid-template-columns:minmax(0,1.45fr) minmax(275px,.75fr);gap:18px;align-items:start}.panel{background:linear-gradient(145deg,rgba(27,36,48,.96),rgba(17,23,31,.96));border:1px solid var(--line);border-radius:16px;box-shadow:var(--shadow)}.panel-head{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;padding:22px 23px 0}.panel h3{font-size:17px;margin:0;letter-spacing:0}.sub{font-size:12px;color:var(--muted);margin:5px 0 0}.upload-panel{padding-bottom:22px}.drop{margin:20px 22px 14px;border:1px dashed #506173;border-radius:13px;min-height:220px;padding:28px 20px;display:flex;align-items:center;justify-content:center;text-align:center;transition:.18s ease;background:rgba(12,17,23,.38)}.drop:hover,.drop.drag{border-color:var(--accent);background:rgba(155,227,192,.06);transform:translateY(-1px)}.drop.ready{border-style:solid;border-color:#477762}.drop-icon{width:48px;height:48px;border-radius:14px;background:#223239;color:var(--accent);display:grid;place-items:center;font-size:25px;margin:0 auto 13px}.drop strong{display:block;font-size:15px}.drop span{display:block;color:var(--muted);font-size:12px;margin-top:6px}.choose{margin-top:17px;border:1px solid #59706f;border-radius:9px;background:#20302f;color:var(--accent);padding:9px 14px;font-weight:650}.choose:hover{background:#2a403d}.file-info{display:none;margin:0 22px 16px;padding:12px 14px;border:1px solid #334555;border-radius:10px;background:#131c25;align-items:center;justify-content:space-between;gap:15px}.file-info.show{display:flex}.file-copy{min-width:0}.file-name{font-weight:650;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.file-meta{font-size:12px;color:var(--muted);margin-top:3px}.clear{border:0;background:transparent;color:var(--muted);font-size:20px;line-height:1;padding:3px 7px;min-width:40px;min-height:40px}.clear:hover{color:var(--text)}.actions{display:flex;align-items:center;gap:12px;margin:0 22px}.primary{border:0;border-radius:9px;padding:11px 17px;min-height:44px;color:#10221a;background:var(--accent);font-weight:750;box-shadow:0 7px 18px rgba(155,227,192,.13);transition:.18s ease}.primary:hover:not(:disabled){background:#b9f0d4;transform:translateY(-1px)}.primary:disabled{opacity:.42;cursor:not-allowed}.progress-wrap{flex:1;min-width:120px}.progress-track{height:7px;border-radius:99px;background:#2b3743;overflow:hidden}.progress-bar{width:0;height:100%;background:var(--accent2);border-radius:inherit;transition:width .16s ease}.progress-label{font-size:11px;color:var(--muted);margin-top:5px;display:block}.result{min-height:20px;margin:13px 22px 0;font-size:13px;color:var(--muted)}.result.ok{color:var(--accent)}.result.error{color:var(--danger)}
.side{display:grid;gap:18px}.status-panel{padding:22px 22px 20px}.status-top{display:flex;justify-content:space-between;align-items:center}.refresh{border:1px solid var(--line);border-radius:8px;color:var(--muted);background:#17212b;width:32px;height:30px;font-size:17px}.refresh:hover{color:var(--text);border-color:#65778a}.metrics{margin-top:20px;display:grid;gap:12px}.metric{display:flex;justify-content:space-between;gap:15px;padding-bottom:12px;border-bottom:1px solid #27333f}.metric:last-child{border-bottom:0;padding-bottom:0}.metric dt{color:var(--muted);font-size:12px}.metric dd{margin:0;text-align:right;font-weight:650;font-size:13px;max-width:175px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.image-state{margin-top:18px;border-radius:10px;padding:12px 13px;background:#12221d;border:1px solid #2d513e;color:#b8ebcf;font-size:12px}.image-state.empty{background:#211e18;border-color:#55432b;color:#eac18f}.api-panel{padding:22px}.api-code{position:relative;margin-top:16px}.api-code code{display:block;border:1px solid #303e4c;border-radius:9px;background:#10161d;color:#cbd7e4;padding:12px 42px 12px 12px;font:11px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;overflow:auto;white-space:nowrap}.copy{position:absolute;right:7px;top:7px;border:1px solid #394b5d;border-radius:6px;background:#1b2733;color:var(--muted);padding:5px 8px;font-size:11px}.copy:hover{color:var(--text);border-color:#6a7f92}.api-note{color:var(--muted);font-size:12px;margin:11px 0 0}.footer{display:flex;justify-content:space-between;gap:16px;margin-top:27px;color:#68788a;font-size:11px}.footer code{color:#9aabbc}
@media(max-width:780px){.shell{padding:24px 14px 40px}.topbar{margin-bottom:28px}.layout{grid-template-columns:1fr}.intro h2{font-size:34px}.drop{min-height:195px}.footer{flex-direction:column;gap:5px}}
@media(max-width:560px){body{overflow-x:hidden}.shell{padding-top:max(12px,env(safe-area-inset-top));padding-right:max(12px,env(safe-area-inset-right));padding-bottom:max(24px,env(safe-area-inset-bottom));padding-left:max(12px,env(safe-area-inset-left))}.topbar{align-items:flex-start;gap:10px;margin-bottom:18px}.brand{gap:8px}.brand-mark{width:32px;height:32px;border-radius:8px;font-size:16px}.brand h1{font-size:16px}.brand p{font-size:10px;margin-top:2px}.online{font-size:10px;padding:5px 7px;gap:5px}.online i{width:5px;height:5px}.intro{margin-bottom:14px}.intro h2{font-size:26px;line-height:1.08;margin:5px 0 6px}.intro p{font-size:12px;line-height:1.4}.panel{border-radius:11px}.panel-head{padding:14px 13px 0;gap:5px}.panel h3{font-size:15px}.sub{font-size:11px;margin-top:3px}.drop{margin:13px 13px 10px;min-height:135px;padding:15px 10px}.drop-icon{width:35px;height:35px;border-radius:10px;font-size:19px;margin-bottom:8px}.drop strong{font-size:13px}.drop span{font-size:11px;margin-top:3px}.choose{margin-top:10px;padding:7px 11px;min-height:38px;font-size:12px}.file-info,.actions,.result{margin-left:13px;margin-right:13px}.file-info{padding:8px 10px;margin-bottom:10px}.file-name{font-size:13px}.file-meta{font-size:11px;margin-top:1px}.clear{min-width:36px;min-height:36px}.actions{align-items:stretch;flex-direction:column;gap:7px}.primary{width:100%;min-height:40px;padding:8px 12px;font-size:13px}.progress-wrap{min-width:0}.progress-track{height:6px}.progress-label{font-size:10px;margin-top:3px}.result{font-size:11px;margin-top:8px;min-height:17px}.status-panel,.api-panel{padding:14px 13px}.status-top{align-items:center}.metrics{margin-top:12px;gap:8px}.metric{padding-bottom:8px;gap:8px}.metric dt,.metric dd{font-size:11px}.metric dd{max-width:58%}.image-state{margin-top:12px;padding:9px 10px;font-size:11px}.refresh{width:36px;height:36px;font-size:15px}.api-code{margin-top:10px}.api-code code{font-size:9px;padding:9px 38px 9px 9px}.copy{top:5px;right:5px;min-height:28px;padding:3px 6px;font-size:10px}.api-note{font-size:10px;margin-top:7px}.footer{margin-top:14px;font-size:10px}}
@media(max-width:360px){.topbar{align-items:center;flex-direction:row}.online{align-self:auto}.intro h2{font-size:24px}.intro p{font-size:11px}.drop{min-height:125px}.panel-head{padding-top:13px}.status-top{align-items:flex-start}.metric{gap:7px}.metric dd{max-width:62%}}
/* Mobile-first visual skin. */
:root{color-scheme:light;--bg:#edf3f1;--surface:#fff;--soft:#f5f8f7;--line:#d6e2df;--text:#18252b;--muted:#64747b;--accent:#176b58;--accent-soft:#e0f1eb;--gold:#a8601c;--danger:#b6453e;--shadow:0 8px 24px rgba(25,57,51,.08)}
body{background:var(--bg);color:var(--text);font-size:14px}
.shell{max-width:760px;padding:16px 12px 28px}
.topbar{margin-bottom:20px;gap:12px}.brand{gap:9px}.brand-mark{width:34px;height:34px;border:0;border-radius:10px;background:var(--accent);color:#fff;font-size:16px;box-shadow:none}.brand h1{font-size:17px}.brand p{color:var(--muted);font-size:11px;margin-top:2px}.online{border-color:#b8d9ce;background:#f2faf7;color:var(--accent);font-size:10px;padding:5px 8px;gap:5px}.online i{width:6px;height:6px;background:var(--accent);box-shadow:none}
.intro{margin-bottom:14px}.eyebrow{font-size:10px;color:var(--gold)}.intro h2{font-size:25px;line-height:1.1;margin:5px 0 6px;max-width:520px}.intro p{color:var(--muted);font-size:12px;line-height:1.4}
.layout{grid-template-columns:1fr;gap:12px}.panel{background:var(--surface);border-color:var(--line);border-radius:12px;box-shadow:var(--shadow)}.panel-head{padding:15px 14px 0;gap:6px}.panel h3{font-size:16px}.sub{color:var(--muted);font-size:11px;margin-top:3px}.upload-panel{padding-bottom:16px}.drop{margin:13px 14px 10px;min-height:132px;padding:15px 10px;border-color:#a8c7be;border-radius:10px;background:var(--soft);transform:none}.drop:hover,.drop.drag{border-color:var(--accent);background:var(--accent-soft);transform:none}.drop.ready{border-color:var(--accent);background:#f1faf6}.drop-icon{width:34px;height:34px;border-radius:9px;background:var(--accent-soft);color:var(--accent);font-size:19px;margin-bottom:8px}.drop strong{font-size:13px}.drop span{color:var(--muted);font-size:11px;margin-top:3px}.choose{margin-top:10px;border:0;border-radius:7px;background:var(--accent);color:#fff;padding:7px 12px;min-height:38px;font-size:12px}.choose:hover{background:#115542}.file-info{margin:0 14px 10px;padding:8px 10px;border-color:var(--line);border-radius:8px;background:var(--soft);gap:8px}.file-name{font-size:13px}.file-meta{color:var(--muted);font-size:11px;margin-top:1px}.clear{color:var(--muted);min-width:36px;min-height:36px}.clear:hover{color:var(--text)}.actions{margin:0 14px;align-items:stretch;flex-direction:column;gap:7px}.primary{width:100%;min-height:40px;border-radius:7px;padding:8px 12px;background:var(--accent);color:#fff;font-size:13px;box-shadow:none}.primary:hover:not(:disabled){background:#115542;transform:none}.primary:disabled{background:#9ab8ae}.progress-wrap{min-width:0}.progress-track{height:6px;background:#dce8e4}.progress-bar{background:var(--gold)}.progress-label{color:var(--muted);font-size:10px;margin-top:3px}.result{color:var(--muted);font-size:11px;margin:8px 14px 0;min-height:17px}.result.ok{color:var(--accent)}.result.error{color:var(--danger)}
.side{gap:12px}.status-panel{padding:15px 14px 14px}.status-top{align-items:center}.refresh{width:36px;height:36px;border-color:var(--line);border-radius:8px;background:var(--soft);color:var(--muted);font-size:15px}.refresh:hover{color:var(--accent);border-color:#a8c7be}.metrics{margin-top:12px;gap:8px}.metric{gap:8px;padding-bottom:8px;border-color:#e7efed}.metric dt{color:var(--muted);font-size:11px}.metric dd{color:var(--text);font-size:11px;max-width:58%}.image-state{margin-top:12px;padding:9px 10px;border-color:#b8d9ce;background:#f2faf7;color:var(--accent);font-size:11px}.image-state.empty{background:#fff8ed;border-color:#ecd0a6;color:#93601f}.api-panel{padding:15px 14px}.api-code{margin-top:10px}.api-code code{border-color:var(--line);background:#f7f9f8;color:#36505a;padding:9px 38px 9px 9px;font-size:9px}.copy{top:5px;right:5px;border-color:var(--line);background:#fff;color:var(--muted);min-height:28px;padding:3px 6px;font-size:10px}.copy:hover{color:var(--accent);border-color:#a8c7be}.api-note{color:var(--muted);font-size:10px;margin-top:7px}.footer{margin-top:14px;color:#7b8b90;font-size:10px}.footer code{color:#60747b}
@media(min-width:760px){.shell{max-width:1000px;padding:28px 22px 42px}.topbar{margin-bottom:28px}.intro h2{font-size:34px}.intro p{font-size:13px}.layout{grid-template-columns:minmax(0,1.35fr) minmax(280px,.85fr);gap:16px}.panel-head{padding:18px 18px 0}.drop{min-height:168px;margin:16px 18px 12px}.file-info,.actions,.result{margin-left:18px;margin-right:18px}.actions{flex-direction:row;align-items:center}.primary{width:auto;white-space:nowrap}.status-panel,.api-panel{padding:18px}.footer{font-size:11px;margin-top:20px}}
@media(max-width:360px){.shell{padding-left:10px;padding-right:10px}.intro h2{font-size:23px}.drop{min-height:118px}.panel-head{padding-top:13px}}
.layout,.side,.panel,.panel-head,.api-code{min-width:0}.api-code code{max-width:100%;min-width:0}@media(max-width:759px){.layout{grid-template-columns:minmax(0,1fr)}}
/* Final alignment pass. */
.panel-head{align-items:center}.panel-head>div:first-child{min-width:0}.panel-head .eyebrow{flex:0 0 auto;white-space:nowrap;text-align:right}.topbar .online{margin-left:auto}.drop>div{width:100%;max-width:360px}.file-info{min-width:0}.file-copy{overflow:hidden}.actions{align-items:center}.progress-wrap{min-width:0;width:100%}.status-top{gap:10px}.status-top>div{min-width:0}.metric dt{flex:1;min-width:0}.metric dd{flex-shrink:1}
.preview{display:none;max-width:100%;max-height:150px;width:auto;height:auto;margin:0 auto 9px;border:1px solid var(--line);border-radius:7px;object-fit:contain}.preview.show{display:block}
@media(min-width:760px){.actions .progress-wrap{min-width:160px}.upload-panel,.side{min-width:0}}
@media(max-width:420px){.panel-head .eyebrow{white-space:normal;max-width:86px;font-size:9px;line-height:1.2}.panel-head{gap:10px}.drop>div{max-width:300px}.preview{max-height:92px}}
</style></head><body><main class="shell">
<header class="topbar"><div class="brand"><div class="brand-mark">P</div><div><h1>PaperDIY</h1><p>Image display</p></div></div><div class="online"><i></i>Online</div></header>
<section class="intro"><h2>Send an image.</h2><p>PNG/JPG · 2.4 MiB max</p></section>
<div class="layout"><section class="panel upload-panel"><div class="panel-head"><div><h3>Upload image</h3><p class="sub">PNG or JPG</p></div><div class="eyebrow">2.4 MiB max</div></div>
<div id="drop" class="drop"><div><img id="preview" class="preview" alt="Preview"><div class="drop-icon">&#8593;</div><strong>Drop image</strong><span>PNG/JPG · 2.4 MiB max</span><button id="choose" class="choose" type="button">Choose</button><input id="file" type="file" accept="image/png,image/jpeg" hidden></div></div>
<div id="fileInfo" class="file-info"><div class="file-copy"><div id="fileName" class="file-name"></div><div id="fileMeta" class="file-meta"></div></div><button id="clear" class="clear" type="button" aria-label="Remove selected file">&times;</button></div>
<div class="actions"><button id="send" class="primary" type="button" disabled>Upload &#8594;</button><div class="progress-wrap"><div class="progress-track"><div id="progress" class="progress-bar"></div></div><span id="progressLabel" class="progress-label">Choose image</span></div></div><p id="result" class="result" role="status"></p></section>
<aside class="side"><section class="panel status-panel"><div class="status-top"><div><h3>Status</h3><p class="sub">Display info</p></div><button id="refresh" class="refresh" type="button" aria-label="Refresh status">&#8635;</button></div><dl class="metrics"><div class="metric"><dt>Network</dt><dd id="ssid">PaperDIY-PNG</dd></div><div class="metric"><dt>IP</dt><dd id="ip">192.168.4.1</dd></div><div class="metric"><dt>Image</dt><dd id="dimensions">None</dd></div><div class="metric"><dt>Size</dt><dd id="bytes">-</dd></div></dl><div id="imageState" class="image-state empty">No image</div></section>
<section class="panel api-panel"><h3>API</h3><p class="sub">POST an image</p><div class="api-code"><code id="curl">curl -X POST -H 'Content-Type: image/jpeg' --data-binary @your.jpg http://192.168.4.1/api/push</code><button id="copy" class="copy" type="button">Copy</button></div><p class="api-note"><code>/api/push</code> &middot; 2.4 MiB max</p></section></aside></div>
<footer class="footer"><span>PaperDIY</span><span><code>/api/status</code></span></footer></main>
<script>
const $=id=>document.getElementById(id),fileInput=$('file'),drop=$('drop'),preview=$('preview'),choose=$('choose'),clear=$('clear'),send=$('send'),fileInfo=$('fileInfo'),fileName=$('fileName'),fileMeta=$('fileMeta'),result=$('result'),progress=$('progress'),progressLabel=$('progressLabel');let selected=null,objectUrl=null;
const maxBytes=2.4*1024*1024,maxClientSide=960,compressAt=600*1024;
function bytes(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB'}
function message(text,kind){result.textContent=text;result.className='result'+(kind?' '+kind:'')}
function clearSelection(){selected=null;fileInput.value='';fileInfo.classList.remove('show');drop.classList.remove('ready');send.disabled=true;progress.style.width='0';progressLabel.textContent='Choose image';if(objectUrl){URL.revokeObjectURL(objectUrl);objectUrl=null}preview.removeAttribute('src');preview.classList.remove('show');message('')}
function setFile(file){if(!file)return;const allowed=['image/png','image/jpeg','image/jpg'];if(file.type&&!allowed.includes(file.type)){clearSelection();message('PNG or JPG only.','error');return}if(file.size>maxBytes){clearSelection();message('File exceeds 2.4 MiB.','error');return}selected=file;fileName.textContent=file.name;fileMeta.textContent=bytes(file.size)+' - ready';fileInfo.classList.add('show');drop.classList.add('ready');send.disabled=false;progress.style.width='0';progressLabel.textContent='Ready';message('');if(objectUrl)URL.revokeObjectURL(objectUrl);objectUrl=URL.createObjectURL(file);preview.src=objectUrl;preview.classList.add('show')}
function prepareUpload(file){if(file.size<=compressAt)return Promise.resolve(file);return new Promise(resolve=>{const image=new Image(),url=URL.createObjectURL(file);image.onload=()=>{URL.revokeObjectURL(url);const longest=Math.max(image.naturalWidth,image.naturalHeight),scale=Math.min(1,maxClientSide/longest);if(scale===1){resolve(file);return}const canvas=document.createElement('canvas');canvas.width=Math.max(1,Math.round(image.naturalWidth*scale));canvas.height=Math.max(1,Math.round(image.naturalHeight*scale));const context=canvas.getContext('2d');context.fillStyle='#fff';context.fillRect(0,0,canvas.width,canvas.height);context.drawImage(image,0,0,canvas.width,canvas.height);canvas.toBlob(blob=>{if(!blob||blob.size>=file.size){resolve(file);return}const base=file.name.replace(/\.[^.]+$/,'');resolve(new File([blob],base+'.jpg',{type:'image/jpeg',lastModified:Date.now()}))},'image/jpeg',.82)};image.onerror=()=>{URL.revokeObjectURL(url);resolve(file)};image.src=url})}
choose.onclick=()=>fileInput.click();drop.onclick=e=>{if(e.target!==choose)fileInput.click()};fileInput.onchange=()=>setFile(fileInput.files[0]);clear.onclick=clearSelection;['dragenter','dragover'].forEach(type=>drop.addEventListener(type,e=>{e.preventDefault();drop.classList.add('drag')}));['dragleave','drop'].forEach(type=>drop.addEventListener(type,e=>{e.preventDefault();drop.classList.remove('drag')}));drop.addEventListener('drop',e=>setFile(e.dataTransfer.files[0]));
send.onclick=async()=>{if(!selected)return;send.disabled=true;clear.disabled=true;progress.style.width='0';progressLabel.textContent='Preparing';message('Optimizing large image...');let payload;try{payload=await prepareUpload(selected)}catch(e){payload=selected}if(!payload||!selected){send.disabled=false;clear.disabled=false;return}const xhr=new XMLHttpRequest();xhr.open('POST','/api/push');xhr.setRequestHeader('Content-Type',payload.type==='image/jpeg'?'image/jpeg':'image/png');xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);progress.style.width=p+'%';progressLabel.textContent='Uploading '+p+'%'}};xhr.onload=()=>{send.disabled=false;clear.disabled=false;if(xhr.status>=200&&xhr.status<300){progress.style.width='100%';progressLabel.textContent='Done';message(xhr.responseText||'Displayed.','ok');refreshStatus()}else{progressLabel.textContent='Failed';message(xhr.responseText||('HTTP '+xhr.status),'error')}};xhr.onerror=()=>{send.disabled=false;clear.disabled=false;progressLabel.textContent='Offline';message('Connection failed.','error')};xhr.send(payload)};
async function refreshStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const data=await r.json();$('ssid').textContent=data.ssid||'-';$('ip').textContent=data.ip||'-';if(data.image){$('dimensions').textContent=(data.image.format?data.image.format+' ':'')+data.image.width+' x '+data.image.height;$('bytes').textContent=bytes(data.image.bytes);$('imageState').className='image-state';$('imageState').textContent='Ready'}else{$('dimensions').textContent='None';$('bytes').textContent='-';$('imageState').className='image-state empty';$('imageState').textContent='No image'}}catch(e){$('imageState').className='image-state empty';$('imageState').textContent='Unavailable'}}
function copied(){const b=$('copy');b.textContent='Copied';setTimeout(()=>b.textContent='Copy',1400)};$('refresh').onclick=refreshStatus;$('copy').onclick=()=>{const text=$('curl').textContent;if(navigator.clipboard){navigator.clipboard.writeText(text).then(copied).catch(()=>message('Copy is unavailable in this browser.','error'));return}const area=document.createElement('textarea');area.value=text;area.style.position='fixed';area.style.opacity='0';document.body.appendChild(area);area.select();try{document.execCommand('copy')?copied():message('Copy is unavailable in this browser.','error')}catch(e){message('Copy is unavailable in this browser.','error')}area.remove()};refreshStatus();
</script></body></html>)HTML";
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(request, "Pragma", "no-cache");
  httpd_resp_set_hdr(request, "Expires", "0");
  return send_text(request, "text/html; charset=utf-8", kPage);
}

esp_err_t handle_status(httpd_req_t* request) {
  ImageInfo info;
  const char* error = nullptr;
  char body[256];
  if (inspect_image(kImagePath, &info, &error)) {
    std::snprintf(body, sizeof(body),
                  "{\"ssid\":\"%s\",\"ip\":\"192.168.4.1\",\"image\":{\"format\":\"%s\",\"width\":%lu,\"height\":%lu,\"bytes\":%u}}",
                  kApSsid, format_name(info.format),
                  static_cast<unsigned long>(info.width),
                  static_cast<unsigned long>(info.height),
                  static_cast<unsigned>(info.bytes));
  } else {
    std::snprintf(body, sizeof(body),
                  "{\"ssid\":\"%s\",\"ip\":\"192.168.4.1\",\"image\":null}",
                  kApSsid);
  }
  return send_text(request, "application/json; charset=utf-8", body);
}

esp_err_t handle_push(httpd_req_t* request) {
  if (request->content_len == 0 || request->content_len > kMaxImageBytes) {
    return send_error(request, "400 Bad Request", "image body is empty or larger than 2.4 MiB");
  }

  UploadState state;
  if (!start_upload(&state, request->content_len)) {
    return send_error(request, "500 Internal Server Error", state.error);
  }

  tune_upload_socket(request);
  const int64_t receive_started = esp_timer_get_time();
  size_t remaining = request->content_len;
  while (remaining > 0) {
    const size_t wanted = std::min(remaining, kReceiveBufferSize);
    const int received = httpd_req_recv(
        request, reinterpret_cast<char*>(state.data + state.bytes), wanted);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (received <= 0) {
      state.failed = true;
      state.error = "HTTP body receive failed";
      break;
    }
    state.bytes += static_cast<size_t>(received);
    remaining -= static_cast<size_t>(received);
  }

  if (remaining != 0 && !state.failed) {
    state.failed = true;
    state.error = "HTTP body is incomplete";
  }
  const int64_t receive_finished = esp_timer_get_time();
  const uint64_t receive_ms = static_cast<uint64_t>(
      (receive_finished - receive_started) / 1000);
  const uint32_t receive_kbps = receive_ms == 0
                                    ? 0
                                    : static_cast<uint32_t>(
                                          (static_cast<uint64_t>(state.bytes) * 1000) /
                                          (receive_ms * 1024));
  ESP_LOGI(kTag, "HTTP receive: bytes=%u time=%llums rate=%u KiB/s",
           static_cast<unsigned>(state.bytes),
           static_cast<unsigned long long>(receive_ms), receive_kbps);
  finish_upload(&state);
  ESP_LOGI(kTag, "HTTP raw upload: accepted=%s bytes=%u",
           state.accepted ? "yes" : "no", static_cast<unsigned>(state.bytes));
  if (!state.accepted) {
    return send_error(request, "400 Bad Request",
                      state.error != nullptr ? state.error : "PNG/JPG rejected");
  }

  ESP_LOGI(kTag, "display: decoding uploaded %s", format_name(state.info.format));
  if (!display_image_async(state.info)) {
    return send_error(request, "422 Unprocessable Entity",
                      "image saved but could not be displayed");
  }

  char result[128];
  std::snprintf(result, sizeof(result), "Displayed %lux%lu %s",
                static_cast<unsigned long>(state.info.width),
                static_cast<unsigned long>(state.info.height),
                format_name(state.info.format));
  return send_text(request, "text/plain; charset=utf-8", result);
}

esp_err_t handle_legacy_upload(httpd_req_t* request) {
  return send_error(request, "400 Bad Request",
                    "Use POST /api/push with Content-Type: image/png or image/jpeg");
}

void init_spiffs() {
  const esp_vfs_spiffs_conf_t config = {
      .base_path = kFsBase,
      .partition_label = "spiffs",
      .max_files = 4,
      .format_if_mount_failed = true,
  };
  ESP_ERROR_CHECK(esp_vfs_spiffs_register(&config));
  size_t total = 0;
  size_t used = 0;
  ESP_ERROR_CHECK(esp_spiffs_info("spiffs", &total, &used));
  ESP_LOGI(kTag, "SPIFFS mounted: used=%u total=%u", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
}

void init_wifi_ap() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  wifi_config_t ap_config = {};
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), kApSsid,
               sizeof(ap_config.ap.ssid));
  std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), kApPassword,
               sizeof(ap_config.ap.password));
  ap_config.ap.ssid_len = std::strlen(kApSsid);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_protocol(
      WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
  const esp_err_t bandwidth_result =
      esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
  if (bandwidth_result != ESP_OK) {
    ESP_LOGW(kTag, "AP HT40 unavailable (%s), using HT20",
             esp_err_to_name(bandwidth_result));
  }
  ESP_LOGI(kTag, "AP ready: SSID=%s IP=192.168.4.1", kApSsid);
}

void start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_open_sockets = 4;
  config.max_uri_handlers = 8;
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;
  config.lru_purge_enable = true;
  ESP_ERROR_CHECK(httpd_start(&s_http, &config));

  static const httpd_uri_t index_uri = {
      .uri = "/", .method = HTTP_GET, .handler = handle_index, .user_ctx = nullptr};
  static const httpd_uri_t status_uri = {
      .uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = nullptr};
  static const httpd_uri_t push_uri = {
      .uri = "/api/push", .method = HTTP_POST, .handler = handle_push, .user_ctx = nullptr};
  static const httpd_uri_t legacy_upload_uri = {
      .uri = "/upload", .method = HTTP_POST, .handler = handle_legacy_upload, .user_ctx = nullptr};
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_http, &index_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_http, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_http, &push_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_http, &legacy_upload_uri));
  ESP_LOGI(kTag, "HTTP server started: http://192.168.4.1/");
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t nvs_result = nvs_flash_init();
  if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_result);

  auto config = M5.config();
  config.clear_display = false;
  config.fallback_board = m5::board_t::board_M5PaperDIY;
  M5.begin(config);
  M5.Display.setBrightness(160);

  init_spiffs();
  init_wifi_ap();
  start_display_worker();

  ImageInfo stored_image;
  const char* image_error = nullptr;
  if (inspect_image(kImagePath, &stored_image, &image_error)) {
    display_image_async(stored_image);
  }

  start_http_server();
  while (true) {
    M5.update();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
