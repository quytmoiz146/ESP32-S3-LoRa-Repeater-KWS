#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
//  Ring Buffer trên PSRAM cho audio pipeline
//  - Ghi bởi taskI2SCapture (liên tục)
//  - Đọc bởi taskKeywordSpot (khi được đánh thức)
//  - Bảo vệ bằng mutex
// ============================================================

/// Khởi tạo ring buffer trên PSRAM
/// @param capacitySamples Số mẫu int16_t tối đa (vd: 32000 = 2s @16kHz)
/// @return true nếu cấp phát thành công
bool ringBuffer_init(size_t capacitySamples);

/// Ghi mẫu vào ring buffer (gọi từ taskI2SCapture)
/// Tự động ghi đè dữ liệu cũ nếu buffer đầy (circular)
/// @param samples Con trỏ tới mảng int16_t
/// @param count Số mẫu cần ghi
void ringBuffer_write(const int16_t* samples, size_t count);

/// Đọc N mẫu gần nhất từ ring buffer (gọi từ taskKeywordSpot)
/// Dữ liệu được copy ra buffer đích, không xóa khỏi ring buffer
/// @param dest Buffer đích để copy dữ liệu ra
/// @param count Số mẫu cần đọc (lấy N mẫu gần nhất)
/// @return Số mẫu thực tế đọc được (có thể < count nếu buffer chưa đủ dữ liệu)
size_t ringBuffer_readLatest(int16_t* dest, size_t count);

/// Lấy số mẫu hiện có trong ring buffer
size_t ringBuffer_available();

/// Lấy handle của mutex (dùng cho debug/monitoring)
SemaphoreHandle_t ringBuffer_getMutex();

#endif // RING_BUFFER_H
