#ifndef I2S_CAPTURE_TASK_H
#define I2S_CAPTURE_TASK_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================
//  taskI2SCapture — Core 0, Priority 5
//
//  Luồng hoạt động (Dual Mic Stereo):
//  1. i2s_read() → 512 frame stereo (1024 mẫu int32 interleaved)
//  2. Convert int32 → int16 (áp gain)
//  3. Deinterleave → 2 buffer L/R riêng (512 mẫu mỗi kênh)
//  4. Downmix (L+R)/2 → mono (512 mẫu) — fallback 1 mic nếu hỏng
//  5. Stereo analyze → ILD (hướng) + health check
//  6. Ghi mono vào ring buffer (luôn ghi, kể cả gate OFF)
//  7. tier0_vad_process(mono) → kiểm tra energy
//  8. Nếu gate OFF→ON: xTaskNotifyGive(keywordTaskHandle)
// ============================================================

/// Khởi tạo và tạo taskI2SCapture
void i2sCaptureTask_start(TaskHandle_t keywordTaskHandle);

/// Hàm task (không gọi trực tiếp)
void taskI2SCapture(void *pvParameters);

/// Tạm dừng capture task an toàn (chờ task tự dừng sau khi xong i2s_read hiện tại)
/// Gọi từ loop() trước khi ghi WAV
void i2sCaptureTask_pause();

/// Bật lại capture task sau khi ghi WAV xong
void i2sCaptureTask_resume();

#endif // I2S_CAPTURE_TASK_H
