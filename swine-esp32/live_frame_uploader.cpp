#include "live_frame_uploader.h"
#include "swinetrack_http.h"

namespace swt {

struct UploadCfg {
  String camUrl;
  const char* fnBase;
  const char* deviceId;
  const char* deviceSecret;
  uint32_t intervalMs;
};

static UploadCfg g_cfg;

static void liveFrameTask(void* arg) {
  std::vector<uint8_t> jpeg;
  for (;;) {
    jpeg.clear();
    if (fetchCamera(g_cfg.camUrl, jpeg)) {
      // Minimal payload: JPEG plus empty thermal/reading JSON
      String empty = "{}";
      postMultipart(g_cfg.fnBase, g_cfg.deviceId, g_cfg.deviceSecret,
                    "/ingest-live-frame", jpeg, empty, "");
    }
    jpeg.clear();
    vTaskDelay(pdMS_TO_TICKS(g_cfg.intervalMs));
  }
}

void startLiveFrameUploader(const String& cameraUrl,
                            const char* fnBase,
                            const char* deviceId,
                            const char* deviceSecret,
                            uint32_t intervalMs) {
  g_cfg = {cameraUrl, fnBase, deviceId, deviceSecret, intervalMs};
  xTaskCreatePinnedToCore(liveFrameTask, "live_frame_uploader", 8192, nullptr, 1, nullptr, 1);
}

}  // namespace swt

