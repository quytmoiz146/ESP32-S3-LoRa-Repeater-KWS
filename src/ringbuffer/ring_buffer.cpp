#include "ring_buffer.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <Arduino.h>

// ============================================================
//  Ring Buffer Implementation (PSRAM)
// ============================================================

static int16_t* rbBuffer = nullptr;      // Buffer trên PSRAM
static size_t   rbCapacity = 0;          // Dung lượng tối đa (số mẫu)
static size_t   rbWritePos = 0;          // Vị trí ghi tiếp theo
static size_t   rbCount = 0;            // Số mẫu hiện có (tối đa = capacity)
static SemaphoreHandle_t rbMutex = nullptr;

bool ringBuffer_init(size_t capacitySamples) {
  // Cấp phát trên PSRAM (dung lượng lớn hơn internal RAM)
  rbBuffer = (int16_t*)heap_caps_malloc(capacitySamples * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM);
  if (!rbBuffer) {
    // Fallback: thử internal RAM nếu không có PSRAM
    rbBuffer = (int16_t*)malloc(capacitySamples * sizeof(int16_t));
    if (!rbBuffer) {
      Serial.println("[ERR] Ring buffer allocation FAILED!");
      return false;
    }
    Serial.println("[WARN] Ring buffer allocated on internal RAM (no PSRAM)");
  }

  rbCapacity = capacitySamples;
  rbWritePos = 0;
  rbCount = 0;
  memset(rbBuffer, 0, capacitySamples * sizeof(int16_t));

  rbMutex = xSemaphoreCreateMutex();
  if (!rbMutex) {
    Serial.println("[RB] Mutex creation FAILED!");
    free(rbBuffer);
    rbBuffer = nullptr;
    return false;
  }

  return true;
}

void ringBuffer_write(const int16_t* samples, size_t count) {
  if (!rbBuffer || !rbMutex || count == 0) return;

  xSemaphoreTake(rbMutex, portMAX_DELAY);

  for (size_t i = 0; i < count; i++) {
    rbBuffer[rbWritePos] = samples[i];
    rbWritePos = (rbWritePos + 1) % rbCapacity;
  }

  // Cập nhật số mẫu hiện có (tối đa = capacity)
  rbCount += count;
  if (rbCount > rbCapacity) {
    rbCount = rbCapacity;
  }

  xSemaphoreGive(rbMutex);
}

size_t ringBuffer_readLatest(int16_t* dest, size_t count) {
  if (!rbBuffer || !rbMutex || count == 0) return 0;

  xSemaphoreTake(rbMutex, portMAX_DELAY);

  // Số mẫu thực tế có thể đọc
  size_t actualCount = (count > rbCount) ? rbCount : count;

  if (actualCount == 0) {
    xSemaphoreGive(rbMutex);
    return 0;
  }

  // Tính vị trí bắt đầu đọc: lùi lại actualCount mẫu từ writePos
  size_t readStart;
  if (rbWritePos >= actualCount) {
    readStart = rbWritePos - actualCount;
  } else {
    readStart = rbCapacity - (actualCount - rbWritePos);
  }

  // Copy dữ liệu ra buffer đích, xử lý wrap-around
  for (size_t i = 0; i < actualCount; i++) {
    dest[i] = rbBuffer[(readStart + i) % rbCapacity];
  }

  xSemaphoreGive(rbMutex);
  return actualCount;
}
