#ifndef TIER0_VAD_GATE_H
#define TIER0_VAD_GATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================
//  Tier0 — VAD Energy Gate
//
//  State machine: Calibrating → Inactive ↔ Active
//
//  - Calibrating: đo noise floor ban đầu trong calibrationTimeMs
//  - Inactive: energy < onThreshold → cập nhật noise floor (EMA)
//  - Active: energy >= offThreshold → giữ gate mở, gia hạn holdTimer
//            energy < offThreshold VÀ hết holdTime → chuyển Inactive
// ============================================================

/// Trạng thái VAD gate
typedef enum {
  VAD_STATE_CALIBRATING,
  VAD_STATE_INACTIVE,
  VAD_STATE_ACTIVE
} VadState;

/// Cấu hình VAD
typedef struct {
  float onMultiplier;       // Ngưỡng bật: energy >= noiseFloor * onMultiplier
  float offMultiplier;      // Ngưỡng tắt: energy < noiseFloor * offMultiplier
  uint32_t holdTimeMs;      // Giữ gate mở qua khoảng ngắt quãng (ms)
  float minNoiseFloor;      // Sàn tối thiểu noise floor
  uint32_t calibrationTimeMs; // Thời gian calibration ban đầu (ms)
  float emaAlpha;           // Hệ số EMA cập nhật noise floor
  float slowEmaRatio;       // Tỷ lệ EMA chậm khi ACTIVE (slowAlpha = emaAlpha * ratio)
  uint32_t maxActiveDurationMs; // Thời gian ACTIVE tối đa trước khi force reset (ms)
} VadConfig;

/// Thông tin trạng thái VAD (để query từ task khác)
typedef struct {
  VadState state;
  bool gateActive;
  float noiseFloor;
  float lastEnergy;
  float onThreshold;
  float offThreshold;
} VadStatus;

/// Khởi tạo VAD gate với cấu hình cho trước
void tier0_vad_init(const VadConfig& config);

/// Xử lý 1 khối mẫu PCM int16, trả về true nếu gate đang Active
/// @param samples Buffer PCM int16_t
/// @param count Số mẫu (thường = 512)
/// @return true nếu gate Active (có tín hiệu vượt ngưỡng)
bool tier0_vad_process(const int16_t* samples, size_t count);

/// Lấy trạng thái hiện tại (thread-safe snapshot)
VadStatus tier0_vad_get_status();

/// Kiểm tra xem gate vừa chuyển từ OFF→ON không (edge detection)
/// Trả về true DUY NHẤT 1 lần khi transition xảy ra, sau đó reset
bool tier0_vad_gate_just_opened();

#endif // TIER0_VAD_GATE_H
