#pragma once
#include <Arduino.h>
#include <vector>

namespace swt {

// Start a background task that fetches JPEG frames from the camera URL and
// uploads them to the backend at the requested interval (ms). This is intended
// for high-throughput live streaming (~30 FPS).
void startLiveFrameUploader(const String& cameraUrl,
                            const char* fnBase,
                            const char* deviceId,
                            const char* deviceSecret,
                            uint32_t intervalMs);

}  // namespace swt

