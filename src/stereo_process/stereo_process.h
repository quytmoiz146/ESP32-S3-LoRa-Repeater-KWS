#ifndef STEREO_PROCESS_H
#define STEREO_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================
//  Stereo Processing — Dual Mic ICS-43434
//
//  Xử lý tín hiệu stereo từ 2 mic dùng chung 1 cổng I2S:
//  1. Tách stereo interleaved → 2 kênh L/R riêng
//  2. Downmix (L+R)/2 → mono cho pipeline AI
//  3. Phân tích chênh lệch kênh: ILD (hướng) + health check
//  4. Dự phòng: nếu 1 mic hỏng, dùng kênh còn lại
//
//  Khoảng cách mic: ~9cm → hỗ trợ cả ILD và ITD
// ============================================================

/// Kết quả phân tích stereo mỗi khối audio
typedef struct {
  float rmsLeft;         // RMS energy kênh trái
  float rmsRight;        // RMS energy kênh phải
  float ildDb;           // ILD = 20*log10(rmsL/rmsR)
                         //   > 0 → nguồn phát ở phía mic trái
                         //   < 0 → nguồn phát ở phía mic phải
                         //   ≈ 0 → ở giữa hoặc xa
  bool  micWarning;      // true nếu phát hiện 1 mic bất thường
  int   weakChannel;     // Kênh yếu: 0=L, 1=R, -1=bình thường
} StereoAnalysis;

/// Khởi tạo stereo processing (reset bộ đếm health check)
void stereo_init();

/// Tách stereo interleaved → 2 buffer L/R riêng
/// @param stereo  Buffer stereo interleaved int16 (L0,R0,L1,R1,...)
/// @param left    Buffer đích kênh trái (frameCount mẫu)
/// @param right   Buffer đích kênh phải (frameCount mẫu)
/// @param frameCount Số frame (= số mẫu mỗi kênh)
void stereo_deinterleave(const int16_t *stereo,
                         int16_t *left, int16_t *right,
                         size_t frameCount);

/// Downmix 2 kênh → mono: output[i] = (left[i] + right[i]) / 2
/// Nếu 1 mic được đánh dấu hỏng, dùng kênh còn lại thay vì trung bình
/// @param left    Buffer kênh trái (frameCount mẫu)
/// @param right   Buffer kênh phải (frameCount mẫu)
/// @param mono    Buffer đích mono (frameCount mẫu)
/// @param frameCount Số mẫu mỗi kênh
void stereo_downmix(const int16_t *left, const int16_t *right,
                    int16_t *mono, size_t frameCount);

/// Phân tích chênh lệch giữa 2 kênh (ILD + health check)
/// Gọi mỗi khối audio (~32ms). Tự tích lũy bộ đếm imbalance.
/// @param left    Buffer kênh trái
/// @param right   Buffer kênh phải
/// @param frameCount Số mẫu mỗi kênh
/// @return Kết quả phân tích (ILD, warning, weakChannel)
StereoAnalysis stereo_analyze(const int16_t *left, const int16_t *right,
                              size_t frameCount);

/// Lấy kết quả phân tích gần nhất (thread-safe snapshot)
StereoAnalysis stereo_getLastAnalysis();

#endif // STEREO_PROCESS_H
