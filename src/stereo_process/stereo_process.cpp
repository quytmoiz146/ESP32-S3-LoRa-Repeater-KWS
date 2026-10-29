#include "stereo_process.h"
#include "config.h"
#include <math.h>
#include <Arduino.h>

// ============================================================
//  Stereo Processing Implementation — Dual Mic ICS-43434
// ============================================================

// Bộ đếm imbalance liên tiếp (cho health check)
static uint32_t imbalanceCounter = 0;
static bool     micFailed = false;     // true khi xác nhận 1 mic hỏng
static int      failedChannel = -1;    // 0=L, 1=R, -1=bình thường

// Kết quả phân tích gần nhất
static StereoAnalysis lastAnalysis = {};

// ============================================================
//  Khởi tạo
// ============================================================
void stereo_init() {
  imbalanceCounter = 0;
  micFailed = false;
  failedChannel = -1;
  lastAnalysis = {};

  Serial.printf("[STEREO] Init — dual mic, distance=%.1fcm, "
                "imbalanceRatio=%.1f, imbalanceLimit=%d\n",
                MIC_DISTANCE_CM, MIC_IMBALANCE_RATIO,
                MIC_IMBALANCE_COUNT_LIMIT);
}

// ============================================================
//  Tách stereo interleaved → 2 buffer L/R
//  Input: [L0, R0, L1, R1, L2, R2, ...]
//  Output: left=[L0,L1,L2,...], right=[R0,R1,R2,...]
// ============================================================
void stereo_deinterleave(const int16_t *stereo,
                         int16_t *left, int16_t *right,
                         size_t frameCount) {
  for (size_t i = 0; i < frameCount; i++) {
    left[i]  = stereo[i * 2];
    right[i] = stereo[i * 2 + 1];
  }
}

// ============================================================
//  Downmix L + R → Mono
//  Bình thường: mono[i] = (L[i] + R[i]) / 2
//  Nếu 1 mic hỏng: dùng kênh còn lại (bypass)
// ============================================================
void stereo_downmix(const int16_t *left, const int16_t *right,
                    int16_t *mono, size_t frameCount) {
  if (micFailed) {
    // Dự phòng: chỉ dùng kênh hoạt động
    const int16_t *good = (failedChannel == 0) ? right : left;
    for (size_t i = 0; i < frameCount; i++) {
      mono[i] = good[i];
    }
  } else {
    // Cộng trung bình 2 kênh → tăng SNR, giảm nhiễu độc lập
    for (size_t i = 0; i < frameCount; i++) {
      mono[i] = (int16_t)(((int32_t)left[i] + (int32_t)right[i]) / 2);
    }
  }
}

// ============================================================
//  Tính RMS energy của 1 kênh
// ============================================================
static float computeChannelRMS(const int16_t *samples, size_t count) {
  if (count == 0) return 0.0f;

  double sumSquares = 0.0;
  for (size_t i = 0; i < count; i++) {
    double s = (double)samples[i];
    sumSquares += s * s;
  }
  return sqrtf((float)(sumSquares / (double)count));
}

// ============================================================
//  Phân tích chênh lệch kênh: ILD + Health Check
// ============================================================
StereoAnalysis stereo_analyze(const int16_t *left, const int16_t *right,
                              size_t frameCount) {
  StereoAnalysis result = {};

  // 1. Tính RMS energy mỗi kênh
  result.rmsLeft  = computeChannelRMS(left, frameCount);
  result.rmsRight = computeChannelRMS(right, frameCount);

  // 2. Tính ILD (Inter-channel Level Difference) — dB
  //    ILD > 0: nguồn phát ở phía mic trái (trái mạnh hơn)
  //    ILD < 0: nguồn phát ở phía mic phải
  //    ILD ≈ 0: ở giữa hoặc xa (trường xa, khác biệt không đáng kể)
  if (result.rmsLeft > MIC_MIN_ENERGY_FOR_ILD &&
      result.rmsRight > MIC_MIN_ENERGY_FOR_ILD) {
    result.ildDb = 20.0f * log10f(result.rmsLeft / result.rmsRight);
  } else {
    result.ildDb = 0.0f;  // Quá yên, không tính ILD
  }

  // 3. Health check — phát hiện mic hỏng
  //    Kiểm tra tỷ lệ chênh lệch: nếu max/min > threshold
  //    liên tục nhiều khối → 1 mic có vấn đề
  float maxRms = fmaxf(result.rmsLeft, result.rmsRight);
  float minRms = fminf(result.rmsLeft, result.rmsRight);

  bool currentImbalance = false;
  if (minRms > 1.0f) {
    // Cả 2 mic đều có tín hiệu → kiểm tra ratio
    float ratio = maxRms / minRms;
    currentImbalance = (ratio > MIC_IMBALANCE_RATIO);
  } else if (maxRms > MIC_MIN_ENERGY_FOR_ILD) {
    // 1 mic gần như im lặng nhưng mic kia có tín hiệu → bất thường
    currentImbalance = true;
  }
  // Nếu cả 2 đều im → không kết luận (môi trường yên lặng)

  if (currentImbalance) {
    imbalanceCounter++;
    if (imbalanceCounter >= MIC_IMBALANCE_COUNT_LIMIT && !micFailed) {
      micFailed = true;
      failedChannel = (result.rmsLeft < result.rmsRight) ? 0 : 1;

      Serial.printf("[STEREO] *** MIC WARNING *** Channel %s appears "
                    "failed/blocked! (L=%.1f, R=%.1f, %lu consecutive "
                    "imbalanced blocks)\n",
                    (failedChannel == 0) ? "LEFT" : "RIGHT",
                    result.rmsLeft, result.rmsRight,
                    (unsigned long)imbalanceCounter);
      Serial.println("[STEREO] Switching to single-mic fallback mode");
    }
  } else {
    // Tín hiệu bình thường → reset bộ đếm
    if (imbalanceCounter > 0) {
      imbalanceCounter = 0;
    }
    // Nếu trước đó đã báo mic hỏng mà giờ bình thường → phục hồi
    if (micFailed) {
      micFailed = false;
      failedChannel = -1;
      Serial.println("[STEREO] Mic recovered — back to dual-mic mode");
    }
  }

  result.micWarning = micFailed;
  result.weakChannel = failedChannel;

  // Lưu kết quả gần nhất
  lastAnalysis = result;

  return result;
}

// ============================================================
//  Lấy kết quả phân tích gần nhất
// ============================================================
StereoAnalysis stereo_getLastAnalysis() {
  return lastAnalysis;
}
