#include "keyword_spot.h"
#include "config.h"
#include "../tier0/tier0_vad_gate.h"
#include "../ringbuffer/ring_buffer.h"
#include "../stereo_process/stereo_process.h"
#include "../lora_mesh/lora_mesh.h"

#include <Arduino.h>
#include <SD.h>

// Edge Impulse inference SDK
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

// ============================================================
//  Keyword Spotting Task — Edge Impulse Inference
//
//  Model: KWS_v3
//  Labels: "bawl", "noise", "speech"
//  Input: 1 giây audio (16000 mẫu PCM int16 @16kHz)
//  Output: 3 classes, quantized int8, EON compiled
//
//  Detection: Margin + Moving Average + State Machine
//  - margin = bawl - max(noise, speech)
//  - Moving average trên N frame gần nhất
//  - IDLE → CANDIDATE (avg > threshold) → CONFIRMED (duration >= min)
// ============================================================

static TaskHandle_t kwsTaskHandle = nullptr;

// Buffer cho cửa sổ audio KWS (cấp phát 1 lần, dùng lại)
static int16_t* kwsAudioWindow = nullptr;

// Buffer float cho EI (EI yêu cầu float input)
static float* kwsFloatBuffer = nullptr;

// ============================================================
//  Callback cung cấp dữ liệu cho EI signal_t
// ============================================================
static float* ei_signal_buffer_ptr = nullptr;

static int ei_audio_signal_get_data(size_t offset, size_t length, float *out_ptr) {
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] = ei_signal_buffer_ptr[offset + i];
  }
  return EIDSP_OK;
}

// ============================================================
//  Khởi tạo KWS
// ============================================================
void keywordSpot_init() {
  // Cấp phát buffer cho cửa sổ audio (int16)
  kwsAudioWindow = (int16_t*)malloc(KWS_WINDOW_SAMPLES * sizeof(int16_t));
  if (!kwsAudioWindow) {
    Serial.println("[KWS] Audio window allocation FAILED!");
    return;
  }

  // Cấp phát buffer float cho EI
  // Dùng PSRAM nếu có (16000 float = 64KB)
  kwsFloatBuffer = (float*)heap_caps_malloc(
      EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!kwsFloatBuffer) {
    // Fallback: internal RAM
    kwsFloatBuffer = (float*)malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
  }
  if (!kwsFloatBuffer) {
    Serial.println("[KWS] Float buffer allocation FAILED!");
    return;
  }

  Serial.printf("[KWS] Init OK — window=%dms (%d samples), margin_thr=%.2f, min_dur=%dms\n",
                KWS_WINDOW_SIZE_MS, KWS_WINDOW_SAMPLES,
                KWS_MARGIN_THRESHOLD, KWS_MIN_DURATION_MS);
  Serial.printf("[KWS] EI Model: %s, labels=%d, DSP input=%d, NN input=%d\n",
                EI_CLASSIFIER_PROJECT_NAME,
                EI_CLASSIFIER_LABEL_COUNT,
                EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
                EI_CLASSIFIER_NN_INPUT_FRAME_SIZE);
  Serial.printf("[KWS] Labels:");
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.printf(" [%d]=%s", (int)i, ei_classifier_inferencing_categories[i]);
  }
  Serial.println();
}

// ============================================================
//  Giải phóng tài nguyên KWS
// ============================================================
void keywordSpot_deinit() {
  if (kwsAudioWindow) {
    free(kwsAudioWindow);
    kwsAudioWindow = nullptr;
  }
  if (kwsFloatBuffer) {
    heap_caps_free(kwsFloatBuffer);
    kwsFloatBuffer = nullptr;
  }
  Serial.println("[KWS] Deinitialized");
}

// ============================================================
//  Chạy inference thật bằng Edge Impulse
// ============================================================
KwsResult keywordSpot_runInference(const int16_t* audioBuffer, size_t sampleCount) {
  KwsResult result;
  result.score = 0.0f;
  result.label = "unknown";
  result.inferenceMs = 0;
  memset(result.scores, 0, sizeof(result.scores));

  uint32_t startMs = millis();

  // 1. Chuyển int16 → float (normalize về [-1.0, 1.0])
  //    EI mong đợi giá trị raw PCM, MFCC block tự xử lý normalization
  for (size_t i = 0; i < sampleCount && i < (size_t)EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
    kwsFloatBuffer[i] = (float)audioBuffer[i];
  }
  // Pad zeros nếu sampleCount < DSP_INPUT_FRAME_SIZE
  for (size_t i = sampleCount; i < (size_t)EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
    kwsFloatBuffer[i] = 0.0f;
  }

  // 2. Tạo signal_t cho EI
  ei_signal_buffer_ptr = kwsFloatBuffer;
  signal_t signal;
  signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
  signal.get_data = &ei_audio_signal_get_data;

  // 3. Chạy classifier
  ei_impulse_result_t ei_result = {0};
  EI_IMPULSE_ERROR err = run_classifier(&signal, &ei_result, false /* debug */);

  if (err != EI_IMPULSE_OK) {
    Serial.printf("[KWS] run_classifier error: %d\n", (int)err);
    result.inferenceMs = millis() - startMs;
    return result;
  }

  // 4. Tìm label có score cao nhất + fill scores[]
  float maxScore = 0.0f;
  const char* maxLabel = "unknown";
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT && ix < KWS_MAX_LABELS; ix++) {
    result.scores[ix] = ei_result.classification[ix].value;
    if (ei_result.classification[ix].value > maxScore) {
      maxScore = ei_result.classification[ix].value;
      maxLabel = ei_result.classification[ix].label;
    }
  }

  result.score = maxScore;
  result.label = maxLabel;
  result.inferenceMs = millis() - startMs;

  return result;
}

// ============================================================
//  Ghi log detection event vào CSV
// ============================================================
static void logDetectionCSV(const KwsResult& result) {
  bool isNew = !SD.exists(DETECTION_CSV_FILENAME);
  File csvFile = SD.open(DETECTION_CSV_FILENAME, FILE_APPEND);
  if (!csvFile) {
    Serial.println("[KWS] Cannot open detection CSV");
    return;
  }

  if (isNew) {
    csvFile.println("timestamp_ms,label,score,inference_ms");
  }

  csvFile.printf("%lu,%s,%.4f,%lu\n",
                 (unsigned long)millis(),
                 result.label,
                 result.score,
                 (unsigned long)result.inferenceMs);
  csvFile.close();
}

// ============================================================
//  Lưu audio window thành file WAV khi detect keyword
//  Để kiểm tra tính chính xác của model
// ============================================================
static uint32_t detectionFileIndex = 0;

static void saveDetectionWav(const int16_t* audioBuffer, size_t sampleCount,
                              const KwsResult& result) {
  // Tạo thư mục nếu chưa có
  if (!SD.exists("/detections")) {
    SD.mkdir("/detections");
  }

  // Tìm index tiếp theo (tránh ghi đè file cũ khi reboot)
  if (detectionFileIndex == 0) {
    char checkPath[64];
    while (true) {
      snprintf(checkPath, sizeof(checkPath), "/detections/det_%04lu.wav",
               detectionFileIndex);
      if (!SD.exists(checkPath)) break;
      detectionFileIndex++;
    }
  }

  char filename[64];
  snprintf(filename, sizeof(filename), "/detections/det_%04lu.wav",
           detectionFileIndex);

  File wavFile = SD.open(filename, FILE_WRITE);
  if (!wavFile) {
    Serial.printf("[KWS] Cannot create %s\n", filename);
    return;
  }

  // WAV header (44 bytes)
  uint32_t dataSize = sampleCount * sizeof(int16_t);
  uint32_t fileSize = 36 + dataSize;
  uint8_t header[44];
  // RIFF chunk
  memcpy(header, "RIFF", 4);
  memcpy(header + 4, &fileSize, 4);
  memcpy(header + 8, "WAVE", 4);
  // fmt sub-chunk
  memcpy(header + 12, "fmt ", 4);
  uint32_t fmtSize = 16;
  memcpy(header + 16, &fmtSize, 4);
  uint16_t audioFormat = 1; // PCM
  memcpy(header + 20, &audioFormat, 2);
  uint16_t numChannels = 1;
  memcpy(header + 22, &numChannels, 2);
  uint32_t sampleRate = I2S_SAMPLE_RATE;
  memcpy(header + 24, &sampleRate, 4);
  uint32_t byteRate = I2S_SAMPLE_RATE * 2;
  memcpy(header + 28, &byteRate, 4);
  uint16_t blockAlign = 2;
  memcpy(header + 32, &blockAlign, 2);
  uint16_t bitsPerSample = 16;
  memcpy(header + 34, &bitsPerSample, 2);
  // data sub-chunk
  memcpy(header + 36, "data", 4);
  memcpy(header + 40, &dataSize, 4);

  wavFile.write(header, 44);
  wavFile.write((const uint8_t*)audioBuffer, dataSize);
  wavFile.close();

  Serial.printf("[KWS] Saved %s (%s %.1f%%)\n",
                filename, result.label, result.score * 100.0f);
  detectionFileIndex++;
}

// ============================================================
//  Task chính — chờ VAD notify, đọc ring buffer, chạy KWS
//  Cơ chế mới: Margin + Moving Average + State Machine
//  - margin = bawl_score - max(noise_score, speech_score)
//  - Moving average trên KWS_MA_WINDOW_N frame gần nhất
//  - State machine: IDLE → CANDIDATE → CONFIRMED
//  - CANDIDATE cần duy trì avg_margin > threshold liên tục
//    đủ KWS_MIN_DURATION_MS mới confirm
// ============================================================

// State machine cho detection
enum DetectState { DETECT_IDLE, DETECT_CANDIDATE };

void taskKeywordSpot(void* pvParameters) {
  Serial.printf("[KWS_TASK] Started on Core %d\n", xPortGetCoreID());
  Serial.printf("[KWS_TASK] Sliding Window: %dms window, %dms stride (%.0f%% overlap)\n",
                KWS_WINDOW_SIZE_MS, KWS_SLIDE_MS,
                (1.0f - (float)KWS_SLIDE_MS / KWS_WINDOW_SIZE_MS) * 100.0f);
  Serial.printf("[KWS_TASK] Detection: margin_thr=%.2f, MA_window=%d, "
                "min_dur=%dms, cooldown=%dms\n",
                KWS_MARGIN_THRESHOLD, KWS_MA_WINDOW_N,
                KWS_MIN_DURATION_MS, KWS_COOLDOWN_MS);

  while (true) {
    // 1. Ngủ chờ — tốn 0% CPU cho đến khi VAD đánh thức
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.println("[KWS_TASK] Woken by VAD — starting inference loop");

    // --- Reset state mỗi lần VAD gate mở ---
    DetectState detectState = DETECT_IDLE;
    uint32_t candidateStartMs = 0;
    uint32_t cooldownUntilMs = 0;
    bool windowFilled = false;

    // 2. Chạy inference liên tục trong khi gate còn Active
    while (tier0_vad_get_status().gateActive) {

      if (!windowFilled) {
        // === Lần đầu: đọc đầy cửa sổ 1 giây ===
        size_t samplesRead = ringBuffer_readLatest(kwsAudioWindow, KWS_WINDOW_SAMPLES);
        if (samplesRead < KWS_WINDOW_SAMPLES) {
          Serial.printf("[KWS_TASK] Buffer not full yet (%u/%d), waiting...\n",
                        (unsigned)samplesRead, KWS_WINDOW_SAMPLES);
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        windowFilled = true;
      } else {
        // === Các lần sau: trượt cửa sổ tiến SLIDE_SAMPLES ===
        vTaskDelay(pdMS_TO_TICKS(KWS_SLIDE_MS));

        if (!tier0_vad_get_status().gateActive) break;

        memmove(kwsAudioWindow,
                kwsAudioWindow + KWS_SLIDE_SAMPLES,
                (KWS_WINDOW_SAMPLES - KWS_SLIDE_SAMPLES) * sizeof(int16_t));

        int16_t* newPortion = kwsAudioWindow + (KWS_WINDOW_SAMPLES - KWS_SLIDE_SAMPLES);
        ringBuffer_readLatest(newPortion, KWS_SLIDE_SAMPLES);
      }

      // Kiểm tra cooldown
      uint32_t nowMs = millis();
      if (nowMs < cooldownUntilMs) {
        continue;
      }

      // --- Chạy inference ---
      KwsResult result = keywordSpot_runInference(kwsAudioWindow, KWS_WINDOW_SAMPLES);

      // --- Tính margin = bawl - max(noise, speech) ---
      // Thứ tự labels trong EI: bawl=0, noise=1, speech=2
      float bawlScore = result.scores[0];
      float noiseScore = result.scores[1];
      float speechScore = result.scores[2];
      float otherMax = fmaxf(noiseScore, speechScore);
      float margin = bawlScore - otherMax;

      // --- State machine ---
      const char* stateStr = "IDLE";
      uint32_t candidateDurMs = 0;

      switch (detectState) {
        case DETECT_IDLE:
          if (margin > KWS_MARGIN_THRESHOLD) {
            if (KWS_MIN_DURATION_MS > 0) {
              detectState = DETECT_CANDIDATE;
              candidateStartMs = nowMs;
              stateStr = "CANDIDATE";
            } else {
              stateStr = "CONFIRMED"; // Kích hoạt ngay lập tức
            }
          }
          break;

        case DETECT_CANDIDATE:
          candidateDurMs = nowMs - candidateStartMs;
          if (margin <= KWS_MARGIN_THRESHOLD) {
            // Margin rớt ngưỡng → reset ngay (không có quán tính)
            detectState = DETECT_IDLE;
            stateStr = "IDLE (reset)";
          } else if (candidateDurMs >= KWS_MIN_DURATION_MS) {
            // Duy trì đủ lâu → CONFIRMED!
            stateStr = "CONFIRMED";
          } else {
            stateStr = "CANDIDATE";
          }
          break;
      }

      // --- Log ---
      Serial.printf("[KWS_TASK] b=%.0f%% n=%.0f%% s=%.0f%% | margin=%.2f | %lums | %s",
                    bawlScore * 100.0f, noiseScore * 100.0f, speechScore * 100.0f,
                    margin,
                    (unsigned long)result.inferenceMs,
                    stateStr);
      if (detectState == DETECT_CANDIDATE) {
        Serial.printf(" (%lums/%dms)", (unsigned long)candidateDurMs, KWS_MIN_DURATION_MS);
      }
      Serial.println();

      // --- Trigger khi CONFIRMED ---
      if (strcmp(stateStr, "CONFIRMED") == 0) {
        Serial.printf("[KWS_TASK] *** CONFIRMED: %s (%.1f%%) — margin=%.2f, "
                      "duration=%lums ***\n",
                      result.label, result.score * 100.0f,
                      margin, (unsigned long)candidateDurMs);

        // Báo hướng âm thanh từ dual mic
        StereoAnalysis sa = stereo_getLastAnalysis();
        if (sa.rmsLeft > MIC_MIN_ENERGY_FOR_ILD ||
            sa.rmsRight > MIC_MIN_ENERGY_FOR_ILD) {
          const char *direction;
          if (sa.ildDb > 2.0f) {
            direction = "LEFT mic side";
          } else if (sa.ildDb < -2.0f) {
            direction = "RIGHT mic side";
          } else {
            direction = "CENTER (equal)";
          }
          Serial.printf("[KWS_TASK] Direction: %s (ILD=%.1fdB, L=%.0f, R=%.0f)\n",
                        direction, sa.ildDb, sa.rmsLeft, sa.rmsRight);
        }

        // Ghi log vào CSV
        logDetectionCSV(result);

        // Lưu audio WAV
        const size_t fullSamples = RING_BUFFER_SIZE_SAMPLES;
        int16_t* fullBuffer = (int16_t*)heap_caps_malloc(
            fullSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (fullBuffer) {
          size_t got = ringBuffer_readLatest(fullBuffer, fullSamples);
          saveDetectionWav(fullBuffer, got, result);
          heap_caps_free(fullBuffer);
        } else {
          saveDetectionWav(kwsAudioWindow, KWS_WINDOW_SAMPLES, result);
        }

        // Reset state + cooldown
        detectState = DETECT_IDLE;
        cooldownUntilMs = millis() + KWS_COOLDOWN_MS;
        g_alarm_end_ms = millis() + 7000; // Bật còi báo động 7s
        Serial.printf("[KWS_TASK] Cooldown %dms, Alarm 7s\n", KWS_COOLDOWN_MS);

        // --- Gửi SOS qua LoRa Mesh (Định dạng Nhị phân Siêu gọn + QoS ACK + Pin) ---
        {
          DirectionType dirType = DIR_CENTER;
          if (sa.rmsLeft > MIC_MIN_ENERGY_FOR_ILD ||
              sa.rmsRight > MIC_MIN_ENERGY_FOR_ILD) {
            if (sa.ildDb > 2.0f) dirType = DIR_LEFT;
            else if (sa.ildDb < -2.0f) dirType = DIR_RIGHT;
          }
          uint8_t confidence = (uint8_t)(result.score * 100.0f);
          lora_mesh_sendSosBinary(ALERT_BAWL, confidence, dirType, true /* Yêu cầu ACK */);
        }
      }
    }

    // 3. Gate tắt — quay lại ngủ
    Serial.println("[KWS_TASK] VAD gate OFF — going back to sleep");
  }
}

// ============================================================
//  Tạo task
// ============================================================
void keywordSpotTask_start() {
  xTaskCreatePinnedToCore(
    taskKeywordSpot,
    "taskKWS",
    TASK_KWS_STACK,
    NULL,
    TASK_KWS_PRIORITY,
    &kwsTaskHandle,
    TASK_KWS_CORE
  );
  Serial.printf("[KWS_TASK] Created — Core %d, Priority %d, Stack %d\n",
                TASK_KWS_CORE, TASK_KWS_PRIORITY, TASK_KWS_STACK);
}

TaskHandle_t keywordSpotTask_getHandle() {
  return kwsTaskHandle;
}
