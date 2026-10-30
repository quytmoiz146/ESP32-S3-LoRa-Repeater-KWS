#include "i2s_capture_task.h"
#include "config.h"
#include "../tier0/tier0_vad_gate.h"
#include "../ringbuffer/ring_buffer.h"
#include "../stereo_process/stereo_process.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/semphr.h>

// ============================================================
//  taskI2SCapture — Đọc I2S Stereo, downmix mono, ghi ring
//  buffer, chạy VAD, phân tích chênh lệch kênh
// ============================================================

static TaskHandle_t kwsTaskHandle = nullptr;
static TaskHandle_t captureTaskHandle = nullptr;
static float captureGain = FIXED_GAIN;

// Cơ chế pause an toàn: dùng cờ + semaphore thay vì vTaskSuspend
// để tránh deadlock khi task đang nằm trong i2s_read()
static volatile bool pauseRequested = false;
static SemaphoreHandle_t pausedSemaphore = nullptr;  // báo hiệu task đã dừng xong

// ============================================================
//  Buffers tĩnh — tránh stack overflow (tổng ~15KB)
// ============================================================
static int32_t i2sBuf[I2S_SAMPLES_PER_READ];       // 1024 × 4 = 4096 bytes
static int16_t stereoBuf[I2S_SAMPLES_PER_READ];     // 1024 × 2 = 2048 bytes
static int16_t leftBuf[I2S_FRAMES_PER_READ];        // 512 × 2  = 1024 bytes
static int16_t rightBuf[I2S_FRAMES_PER_READ];       // 512 × 2  = 1024 bytes
static int16_t monoBuf[I2S_FRAMES_PER_READ];        // 512 × 2  = 1024 bytes

void taskI2SCapture(void *pvParameters) {
  kwsTaskHandle = (TaskHandle_t)pvParameters;

  size_t bytesRead = 0;

  while (true) {
    // Kiểm tra cờ pause TRƯỚC khi gọi i2s_read()
    // Nếu được yêu cầu dừng → báo hiệu đã dừng, rồi chờ đến khi resume
    if (pauseRequested) {
      Serial.println("[I2S_TASK] Pausing (safe point)");
      xSemaphoreGive(pausedSemaphore);  // Báo: "tôi đã dừng an toàn"
      // Chờ notification để resume
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      Serial.println("[I2S_TASK] Resumed");
      continue;  // Quay lại kiểm tra cờ (phòng trường hợp pause lại ngay)
    }

    // 1. Đọc stereo từ I2S — blocking cho đến khi có đủ dữ liệu
    //    DMA trả về interleaved: [L0, R0, L1, R1, ...]
    esp_err_t err = i2s_read(I2S_PORT, i2sBuf,
                              I2S_SAMPLES_PER_READ * sizeof(int32_t),
                              &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("[I2S_TASK] Read error: %d\n", err);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t samplesRead = bytesRead / sizeof(int32_t);

    // 2. Convert int32 → int16 (dịch phải 16 bit + áp gain + clipping)
    //    Vẫn giữ interleaved format: [L0, R0, L1, R1, ...]
    for (size_t i = 0; i < samplesRead; i++) {
      int16_t raw = (int16_t)(i2sBuf[i] >> 16);
      int32_t amplified = (int32_t)((float)raw * captureGain);
      if (amplified > 32767) amplified = 32767;
      else if (amplified < -32768) amplified = -32768;
      stereoBuf[i] = (int16_t)amplified;
    }

    size_t frameCount = samplesRead / 2;  // Số frame stereo

    // 3. Deinterleave stereo → 2 kênh riêng
    stereo_deinterleave(stereoBuf, leftBuf, rightBuf, frameCount);

    // 4. Downmix (L+R)/2 → mono cho VAD (energy detection)
    //    Nếu 1 mic hỏng, tự động dùng kênh còn lại
    stereo_downmix(leftBuf, rightBuf, monoBuf, frameCount);

    // 5. Phân tích chênh lệch kênh: ILD + health check
    //    Kết quả ILD được đọc bởi KWS task khi detect bawl
    stereo_analyze(leftBuf, rightBuf, frameCount);

    // 6. Ghi mono downmix vào ring buffer cho KWS
    ringBuffer_write(monoBuf, frameCount);

    // 7. Chạy VAD trên mono downmix (tốt hơn cho energy detection)
    tier0_vad_process(monoBuf, frameCount);

    // 8. Nếu gate vừa chuyển OFF→ON: notify KWS task
    if (tier0_vad_gate_just_opened()) {
      if (kwsTaskHandle != nullptr) {
        xTaskNotifyGive(kwsTaskHandle);
      }
    }
  }
}

void i2sCaptureTask_start(TaskHandle_t keywordTaskHandle) {
  // Tạo semaphore cho cơ chế pause
  pausedSemaphore = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(
    taskI2SCapture,
    "taskI2SCapture",
    TASK_I2S_CAPTURE_STACK,
    (void *)keywordTaskHandle,
    TASK_I2S_CAPTURE_PRIORITY,
    &captureTaskHandle,
    TASK_I2S_CAPTURE_CORE
  );
}

void i2sCaptureTask_pause() {
  if (captureTaskHandle == nullptr) return;

  pauseRequested = true;

  // Chờ task xác nhận đã dừng (timeout 2s)
  if (xSemaphoreTake(pausedSemaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
    Serial.println("[I2S_TASK] Capture task paused OK");
  } else {
    Serial.println("[I2S_TASK] WARNING: pause timeout — task may be stuck");
  }

  // Xả DMA buffer cũ để recording bắt đầu sạch
  int32_t flushBuf[256];
  size_t bytesRead;
  for (int i = 0; i < 5; i++) {
    i2s_read(I2S_PORT, flushBuf, sizeof(flushBuf), &bytesRead, portMAX_DELAY);
  }
}

void i2sCaptureTask_resume() {
  if (captureTaskHandle == nullptr) return;

  pauseRequested = false;
  // Đánh thức task (đang chờ ulTaskNotifyTake)
  xTaskNotifyGive(captureTaskHandle);
  Serial.println("[I2S_TASK] Capture task resumed");
}
