#ifndef KEYWORD_SPOT_H
#define KEYWORD_SPOT_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================
//  Keyword Spotting (KWS) — Tier 1
//
//  Nhận diện từ khóa ("bawl" — tiếng kêu cứu)
//  Model: KWS_v1 (Edge Impulse, quantized int8, EON compiler)
//  Input: 1 giây audio PCM int16 @16kHz (16000 mẫu)
//  Output: 3 classes — "bawl", "noise", "speech"
// ============================================================

/// Kết quả inference KWS
#define KWS_MAX_LABELS 3
typedef struct {
  float score;                   // Confidence score cao nhất (0.0 - 1.0)
  const char *label;             // Label: "bawl", "noise", "speech"
  float scores[KWS_MAX_LABELS];  // Confidence từng class [bawl, noise, speech]
  uint32_t inferenceMs;          // Thời gian inference (ms)
} KwsResult;

/// Khởi tạo KWS (load model, allocate buffers)
void keywordSpot_init();

/// Giải phóng tài nguyên KWS
void keywordSpot_deinit();

/// Chạy inference trên buffer audio
/// @param audioBuffer PCM int16_t samples
/// @param sampleCount Số mẫu (thường = KWS_WINDOW_SAMPLES = 16000)
/// @return Kết quả inference
KwsResult keywordSpot_runInference(const int16_t *audioBuffer,
                                   size_t sampleCount);

/// Tạo và start taskKeywordSpot
void keywordSpotTask_start();

/// Hàm task (dùng bởi xTaskCreatePinnedToCore)
void taskKeywordSpot(void *pvParameters);

/// Lấy task handle (để taskI2SCapture notify)
TaskHandle_t keywordSpotTask_getHandle();

#endif // KEYWORD_SPOT_H
