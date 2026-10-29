#include "tier0_vad_gate.h"
#include <math.h>
#include <Arduino.h>

// ============================================================
//  Tier0 VAD Gate Implementation
// ============================================================

static VadConfig  vadCfg;
static VadState   vadState = VAD_STATE_CALIBRATING;
static float      noiseFloor = 0.0f;
static float      lastEnergy = 0.0f;
static uint32_t   holdUntilMs = 0;       // Thời điểm hold hết hạn
static uint32_t   activeStartMs = 0;     // Thời điểm bắt đầu ACTIVE (cho max duration timeout)
static uint32_t   calibStartMs = 0;      // Thời điểm bắt đầu calibration
static float      calibSum = 0.0f;       // Tổng energy trong calibration
static uint32_t   calibCount = 0;        // Số khối đã đo trong calibration
static bool       gateJustOpened = false; // Edge detection flag

// ============================================================
//  Tính RMS energy của 1 khối PCM int16
// ============================================================
static float computeRMS(const int16_t* samples, size_t count) {
  if (count == 0) return 0.0f;

  double sumSquares = 0.0;
  for (size_t i = 0; i < count; i++) {
    double s = (double)samples[i];
    sumSquares += s * s;
  }
  return sqrtf((float)(sumSquares / (double)count));
}

// ============================================================
//  Khởi tạo
// ============================================================
void tier0_vad_init(const VadConfig& config) {
  vadCfg = config;
  vadState = VAD_STATE_CALIBRATING;
  noiseFloor = config.minNoiseFloor;
  lastEnergy = 0.0f;
  holdUntilMs = 0;
  activeStartMs = 0;
  calibStartMs = millis();
  calibSum = 0.0f;
  calibCount = 0;
  gateJustOpened = false;

  Serial.printf("[VAD] Init — onMul=%.1f, offMul=%.1f, hold=%lums, "
                "minFloor=%.1f, calibTime=%lums, slowEmaRatio=%.2f, "
                "maxActive=%lums\n",
                config.onMultiplier, config.offMultiplier,
                (unsigned long)config.holdTimeMs,
                config.minNoiseFloor,
                (unsigned long)config.calibrationTimeMs,
                config.slowEmaRatio,
                (unsigned long)config.maxActiveDurationMs);
  Serial.println("[VAD] Calibrating noise floor...");
}

// ============================================================
//  Xử lý 1 khối mẫu — state machine
// ============================================================
bool tier0_vad_process(const int16_t* samples, size_t count) {
  float energy = computeRMS(samples, count);
  lastEnergy = energy;
  uint32_t nowMs = millis();

  switch (vadState) {
    // ----------------------------------------------------------
    //  CALIBRATING: tích lũy energy để tính noise floor trung bình
    // ----------------------------------------------------------
    case VAD_STATE_CALIBRATING: {
      calibSum += energy;
      calibCount++;

      if (nowMs - calibStartMs >= vadCfg.calibrationTimeMs) {
        // Chốt noise floor
        float avgEnergy = (calibCount > 0) ? (calibSum / (float)calibCount) : vadCfg.minNoiseFloor;
        noiseFloor = (avgEnergy > vadCfg.minNoiseFloor) ? avgEnergy : vadCfg.minNoiseFloor;

        vadState = VAD_STATE_INACTIVE;
        Serial.printf("[VAD] Calibration done — noiseFloor=%.1f "
                      "(from %lu blocks, avg energy=%.1f)\n",
                      noiseFloor, (unsigned long)calibCount, avgEnergy);
      }
      return false; // Gate OFF trong calibration
    }

    // ----------------------------------------------------------
    //  INACTIVE: chờ energy vượt onThreshold
    // ----------------------------------------------------------
    case VAD_STATE_INACTIVE: {
      float onThreshold = noiseFloor * vadCfg.onMultiplier;

      if (energy >= onThreshold) {
        // Transition: Inactive → Active
        vadState = VAD_STATE_ACTIVE;
        holdUntilMs = nowMs + vadCfg.holdTimeMs;
        activeStartMs = nowMs;  // Ghi nhớ thời điểm bắt đầu ACTIVE
        gateJustOpened = true;

        Serial.printf("[VAD] Gate ON — energy=%.1f >= threshold=%.1f "
                      "(floor=%.1f × %.1f)\n",
                      energy, onThreshold, noiseFloor, vadCfg.onMultiplier);
        return true;
      }

      // Cập nhật noise floor bằng EMA (chỉ khi inactive)
      noiseFloor = (1.0f - vadCfg.emaAlpha) * noiseFloor + vadCfg.emaAlpha * energy;
      if (noiseFloor < vadCfg.minNoiseFloor) {
        noiseFloor = vadCfg.minNoiseFloor;
      }
      return false;
    }

    // ----------------------------------------------------------
    //  ACTIVE: giữ gate mở, kiểm tra điều kiện tắt
    // ----------------------------------------------------------
    case VAD_STATE_ACTIVE: {
      float offThreshold = noiseFloor * vadCfg.offMultiplier;

      if (energy >= offThreshold) {
        // Gia hạn hold timer — vẫn có tín hiệu
        holdUntilMs = nowMs + vadCfg.holdTimeMs;
      }

      // ═══ TẦNG 1: Slow EMA — chỉ cập nhật khi energy "trông giống noise" ═══
      // Giữa các câu nói luôn có khoảng lặng ngắn (thở, ngắt câu)
      // → energy tụt xuống gần ambient thật → EMA học từ đó
      if (energy < noiseFloor * vadCfg.onMultiplier) {
        float slowAlpha = vadCfg.emaAlpha * vadCfg.slowEmaRatio;
        noiseFloor = (1.0f - slowAlpha) * noiseFloor + slowAlpha * energy;
        if (noiseFloor < vadCfg.minNoiseFloor) {
          noiseFloor = vadCfg.minNoiseFloor;
        }
      }

      // ═══ TẦNG 2: Max active timeout — chống kẹt vĩnh viễn ═══
      // Không ai nói liên tục 30s không nghỉ → nếu ACTIVE quá lâu thì là noise
      if (nowMs - activeStartMs >= vadCfg.maxActiveDurationMs) {
        vadState = VAD_STATE_INACTIVE;
        // Gán noiseFloor = energy hiện tại (chấp nhận đây là ambient mới)
        noiseFloor = (energy > vadCfg.minNoiseFloor) ? energy : vadCfg.minNoiseFloor;
        Serial.printf("[VAD] Gate OFF — MAX ACTIVE TIMEOUT (%lums) — "
                      "force noiseFloor=%.1f (energy=%.1f)\n",
                      (unsigned long)vadCfg.maxActiveDurationMs,
                      noiseFloor, energy);
        return false;
      }

      // ═══ TẦNG 3: Hold timer hết → tắt gate ═══
      if (energy < offThreshold && nowMs >= holdUntilMs) {
        // Transition: Active → Inactive
        vadState = VAD_STATE_INACTIVE;
        Serial.printf("[VAD] Gate OFF — energy=%.1f < offThreshold=%.1f, "
                      "hold expired\n",
                      energy, offThreshold);
        return false;
      }

      // Vẫn trong hold period hoặc energy >= offThreshold — giữ gate mở
      return true;
    }
  }

  return false; // Không bao giờ tới đây
}

// ============================================================
//  Query trạng thái (snapshot, thread-safe đủ dùng)
// ============================================================
VadStatus tier0_vad_get_status() {
  VadStatus status;
  status.state = vadState;
  status.gateActive = (vadState == VAD_STATE_ACTIVE);
  status.noiseFloor = noiseFloor;
  status.lastEnergy = lastEnergy;
  status.onThreshold = noiseFloor * vadCfg.onMultiplier;
  status.offThreshold = noiseFloor * vadCfg.offMultiplier;
  return status;
}

// ============================================================
//  Edge detection: gate vừa mở (OFF→ON)?
//  Trả true 1 lần duy nhất rồi reset
// ============================================================
bool tier0_vad_gate_just_opened() {
  if (gateJustOpened) {
    gateJustOpened = false;
    return true;
  }
  return false;
}
