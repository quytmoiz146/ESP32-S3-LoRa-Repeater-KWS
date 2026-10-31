#include "config.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <math.h>

// Modules hệ thống 2 tầng
#include "capture/i2s_capture_task.h"
#include "keyword_spot/keyword_spot.h"
#include "lora_mesh/lora_mesh.h"
#include "ringbuffer/ring_buffer.h"
#include "stereo_process/stereo_process.h"
#include "tier0/tier0_vad_gate.h"

// ============================================================
//  Cấu trúc WAV Header (44 bytes)
// ============================================================
struct __attribute__((packed)) WavHeader {
  // Khối RIFF
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize = 0;
  char wave[4] = {'W', 'A', 'V', 'E'};
  // Khối fmt
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1; // PCM
  uint16_t numChannels = 1; // Mono
  uint32_t sampleRate = I2S_SAMPLE_RATE;
  uint32_t byteRate = 0;
  uint16_t blockAlign = 0;
  uint16_t bitsPerSample = 16;
  // Khối data
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize = 0;
};

// ============================================================
//  Biến toàn cục
// ============================================================
static uint32_t recordIndex = 0; // Số thứ tự bản ghi (tăng dần)
static bool sdReady = false;

volatile unsigned long g_alarm_end_ms = 0;

// ============================================================
//  Khởi tạo I2S
// ============================================================
void i2s_init() {
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_config.sample_rate = I2S_SAMPLE_RATE;
  i2s_config.bits_per_sample = I2S_SAMPLE_BITS;
  i2s_config.channel_format = I2S_CHANNEL;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_config.dma_buf_count = I2S_DMA_BUF_COUNT;
  i2s_config.dma_buf_len = I2S_DMA_BUF_LEN;
  i2s_config.use_apll = false; // ESP32-S3 không có APLL
  i2s_config.tx_desc_auto_clear = false;
  i2s_config.fixed_mclk = 0;

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = I2S_BCK_PIN;
  pin_config.ws_io_num = I2S_WS_PIN;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = I2S_DATA_IN_PIN;

  esp_err_t err;
  err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Driver install FAILED: %d\n", err);
    return;
  }
  Serial.println("[I2S] Driver installed OK");

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Set pin FAILED: %d\n", err);
    return;
  }
  Serial.println("[I2S] Pins configured OK");

  i2s_zero_dma_buffer(I2S_PORT);

  // ICS-43434 cần ~200ms để khởi động và ổn định
  Serial.println("[I2S] Waiting 300ms for mic warmup...");
  delay(300);

  // Xả nhiễu khởi động từ bộ đệm DMA
  int32_t flushBuf[256];
  size_t bytesRead;
  for (int i = 0; i < 20; i++) {
    i2s_read(I2S_PORT, flushBuf, sizeof(flushBuf), &bytesRead, portMAX_DELAY);
  }

  Serial.printf("[I2S] Ready — %d Hz, 32-bit read → 16-bit, stereo (dual mic), "
                "FIXED gain=%.2f\n",
                I2S_SAMPLE_RATE, FIXED_GAIN);
}

// ============================================================
//  Khởi tạo thẻ SD
// ============================================================
void sd_init() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("[SD] Mount FAILED!");
    sdReady = false;
    return;
  }

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  Serial.printf(
      "[SD] Mounted OK — Total: %llu MB, Used: %llu MB, Free: %llu MB\n",
      totalBytes / (1024 * 1024), usedBytes / (1024 * 1024),
      (totalBytes - usedBytes) / (1024 * 1024));
  sdReady = true;
}

// ============================================================
//  Khởi tạo thư mục và tìm chỉ số file tiếp theo
// ============================================================
void initFromExistingFiles() {
  if (!SD.exists(WAV_FOLDER)) {
    SD.mkdir(WAV_FOLDER);
    Serial.printf("[FILE] Da tao thu muc %s\n", WAV_FOLDER);
  }

  char path[64];
  while (true) {
    snprintf(path, sizeof(path), "%s/%s_%04lu.wav", WAV_FOLDER, WAV_FILE_PREFIX,
             recordIndex);
    if (!SD.exists(path))
      break;
    recordIndex++;
  }
  Serial.printf("[FILE] Ban ghi tiep theo: #%lu\n", recordIndex);
}

// ============================================================
//  Áp dụng gain lên 1 mẫu, có clipping bảo vệ
// ============================================================
static inline int16_t applyGain(int32_t rawSample32, float gain) {
  int32_t s16 = rawSample32;
  int32_t amplified = (int32_t)((float)s16 * gain);
  if (amplified > 32767)
    amplified = 32767;
  else if (amplified < -32768)
    amplified = -32768;
  return (int16_t)amplified;
}

// ============================================================
//  Ghi thông số bản ghi vào file CSV
// ============================================================
void logToCSV(const char *filename, const WavHeader &hdr, float gainUsed,
              int16_t peakSample, float rmsSample) {
  bool isNew = !SD.exists(CSV_FILENAME);
  File csvFile = SD.open(CSV_FILENAME, FILE_APPEND);
  if (!csvFile) {
    Serial.println("[CSV] Khong the mo file CSV");
    return;
  }

  if (isNew) {
    csvFile.println("stt,ten_file,sample_rate_Hz,bit_depth,num_channels,"
                    "byte_rate,block_align,data_size_bytes,duration_s,"
                    "gain,peak_sample,peak_dbfs,rms_sample,rms_dbfs");
  }

  float duration =
      (hdr.byteRate > 0) ? (float)hdr.dataSize / hdr.byteRate : 0.0f;
  float peakDbfs =
      (peakSample > 0) ? 20.0f * log10f((float)peakSample / 32767.0f) : -120.0f;
  float rmsDbfs =
      (rmsSample > 0.5f) ? 20.0f * log10f(rmsSample / 32767.0f) : -120.0f;

  csvFile.printf("%lu,%s,%lu,%u,%u,%lu,%u,%lu,%.3f,%.2f,%d,%.1f,%.1f,%.1f\n",
                 recordIndex, filename, hdr.sampleRate, hdr.bitsPerSample,
                 hdr.numChannels, hdr.byteRate, hdr.blockAlign, hdr.dataSize,
                 duration, gainUsed, peakSample, peakDbfs, rmsSample, rmsDbfs);
  csvFile.close();
  Serial.printf("[CSV] Da ghi thong so ban ghi #%lu\n", recordIndex);
}

// ============================================================
//  Ghi một file WAV (giữ lại để thu data train AI)
//  LƯU Ý: Hàm này đọc I2S trực tiếp, nên chỉ gọi khi
//  detection tasks CHƯA chạy hoặc đã dừng
// ============================================================
bool recordWavFile() {
  if (!sdReady) {
    Serial.println("[REC] The SD chua san sang");
    return false;
  }

  char filename[64];
  snprintf(filename, sizeof(filename), "%s/%s_%04lu.wav", WAV_FOLDER,
           WAV_FILE_PREFIX, recordIndex);

  Serial.printf("[REC] Ghi %s (%.1f giay, gain=%.2f FIXED)...\n", filename,
                WAV_RECORD_DURATION_SEC, FIXED_GAIN);

  File wavFile = SD.open(filename, FILE_WRITE);
  if (!wavFile) {
    Serial.printf("[REC] Khong the tao %s\n", filename);
    return false;
  }

  // Số mẫu MONO output (sau downmix)
  uint32_t numSamples = (uint32_t)((float)I2S_SAMPLE_RATE * WAV_RECORD_DURATION_SEC);
  uint32_t dataSize = numSamples * 2;

  WavHeader header;
  header.numChannels = 1;
  header.sampleRate = I2S_SAMPLE_RATE;
  header.bitsPerSample = 16;
  header.blockAlign = 2;
  header.byteRate = I2S_SAMPLE_RATE * 2;
  header.dataSize = dataSize;
  header.fileSize = 36 + dataSize;

  wavFile.write((uint8_t *)&header, sizeof(WavHeader));

  const size_t FRAMES_PER_READ = 512;
  const size_t STEREO_SAMPLES = FRAMES_PER_READ * 2; // stereo interleaved
  int32_t *i2sBuf = (int32_t *)malloc(STEREO_SAMPLES * sizeof(int32_t));
  int16_t *stereoBuf = (int16_t *)malloc(STEREO_SAMPLES * sizeof(int16_t));
  int16_t *leftBuf = (int16_t *)malloc(FRAMES_PER_READ * sizeof(int16_t));
  int16_t *rightBuf = (int16_t *)malloc(FRAMES_PER_READ * sizeof(int16_t));
  int16_t *wavBuf =
      (int16_t *)malloc(FRAMES_PER_READ * sizeof(int16_t)); // mono output

  if (!i2sBuf || !stereoBuf || !leftBuf || !rightBuf || !wavBuf) {
    Serial.println("[REC] Cap phat bo nho that bai!");
    if (i2sBuf)
      free(i2sBuf);
    if (stereoBuf)
      free(stereoBuf);
    if (leftBuf)
      free(leftBuf);
    if (rightBuf)
      free(rightBuf);
    if (wavBuf)
      free(wavBuf);
    wavFile.close();
    SD.remove(filename);
    return false;
  }

  uint32_t samplesWritten = 0;
  size_t bytesRead = 0;
  uint32_t startTime = millis();

  int16_t rawMin = 32767, rawMax = -32768;
  int16_t outPeak = 0;
  double sumSquaresOut = 0;
  uint32_t clipCount = 0;  // Đếm số sample bị clipping

  while (samplesWritten < numSamples) {
    size_t framesToRead = FRAMES_PER_READ;
    if (samplesWritten + framesToRead > numSamples) {
      framesToRead = numSamples - samplesWritten;
    }

    // Đọc stereo từ I2S (framesToRead × 2 int32)
    size_t stereoSamples = framesToRead * 2;
    esp_err_t err = i2s_read(I2S_PORT, i2sBuf, stereoSamples * sizeof(int32_t),
                             &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("[REC] Loi doc I2S: %d\n", err);
      break;
    }

    size_t totalSamples = bytesRead / sizeof(int32_t);
    size_t frameCount = totalSamples / 2;

    // Convert int32 → int16 + FIXED gain (vẫn giữ stereo interleaved)
    for (size_t i = 0; i < totalSamples; i++) {
      int16_t raw = (int16_t)(i2sBuf[i] >> 16);
      stereoBuf[i] = applyGain(raw, FIXED_GAIN);
    }

    // Deinterleave → L/R riêng
    stereo_deinterleave(stereoBuf, leftBuf, rightBuf, frameCount);

    // Downmix → mono
    for (size_t i = 0; i < frameCount; i++) {
      wavBuf[i] = (int16_t)(((int32_t)leftBuf[i] + (int32_t)rightBuf[i]) / 2);
    }

    // Thống kê trên mono output
    for (size_t i = 0; i < frameCount; i++) {
      int16_t raw = wavBuf[i];
      if (raw < rawMin)
        rawMin = raw;
      if (raw > rawMax)
        rawMax = raw;

      int16_t absOut = (int16_t)abs(raw);
      if (absOut > outPeak)
        outPeak = absOut;
      if (absOut >= 32767) clipCount++;
      sumSquaresOut += (double)raw * (double)raw;
    }

    wavFile.write((uint8_t *)wavBuf, frameCount * sizeof(int16_t));
    samplesWritten += frameCount;
  }

  free(i2sBuf);
  free(stereoBuf);
  free(leftBuf);
  free(rightBuf);
  free(wavBuf);

  uint32_t actualDataSize = samplesWritten * 2;
  if (samplesWritten != numSamples) {
    header.dataSize = actualDataSize;
    header.fileSize = 36 + actualDataSize;
    wavFile.seek(0);
    wavFile.write((uint8_t *)&header, sizeof(WavHeader));
  }

  wavFile.close();

  uint32_t elapsed = millis() - startTime;
  float rmsOut =
      (samplesWritten > 0) ? sqrtf((float)(sumSquaresOut / samplesWritten)) : 0;
  int16_t rawPeak = max((int16_t)abs(rawMin), (int16_t)abs(rawMax));

  Serial.printf("[REC] Hoan tat %s — %lu mau, %lu bytes, %lu ms\n", filename,
                samplesWritten, actualDataSize, elapsed);
  Serial.printf("[REC] Raw peak=%d | Gain=%.2f (FIXED) | Out peak=%d | Out RMS=%.1f\n",
                rawPeak, FIXED_GAIN, outPeak, rmsOut);
  if (clipCount > 0) {
    Serial.printf("[REC] *** CLIPPING: %lu samples bi cat dinh! Giam FIXED_GAIN ***\n",
                  (unsigned long)clipCount);
  }

  logToCSV(filename, header, FIXED_GAIN, outPeak, rmsOut);

  recordIndex++;
  return true;
}

// ============================================================
//  Khởi tạo hệ thống detection (FreeRTOS tasks)
// ============================================================
bool startDetectionSystem() {
  // 1. Khởi tạo Ring Buffer PSRAM
  if (!ringBuffer_init(RING_BUFFER_SIZE_SAMPLES)) {
    Serial.println("[ERR] Ring buffer init FAILED — cannot start detection");
    return false;
  }

  // 2. Khởi tạo Tier0 VAD
  VadConfig vadConfig;
  vadConfig.onMultiplier = VAD_ON_MULTIPLIER;
  vadConfig.offMultiplier = VAD_OFF_MULTIPLIER;
  vadConfig.holdTimeMs = VAD_HOLD_TIME_MS;
  vadConfig.minNoiseFloor = VAD_MIN_NOISE_FLOOR;
  vadConfig.calibrationTimeMs = VAD_CALIBRATION_TIME_MS;
  vadConfig.emaAlpha = VAD_EMA_ALPHA;
  vadConfig.slowEmaRatio = VAD_SLOW_EMA_RATIO;
  vadConfig.maxActiveDurationMs = VAD_MAX_ACTIVE_DURATION_MS;
  tier0_vad_init(vadConfig);

  // 3. Khởi tạo Stereo Processing (dual mic)
  stereo_init();

  // 4. Khởi tạo KWS
  keywordSpot_init();

  // 5. Tạo tasks theo chiều ngược luồng dữ liệu (đích trước, nguồn sau)
  keywordSpotTask_start();

  TaskHandle_t kwsHandle = keywordSpotTask_getHandle();
  while (kwsHandle == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(10));
    kwsHandle = keywordSpotTask_getHandle();
  }

  i2sCaptureTask_start(kwsHandle);

  // 6. Khởi tạo LoRa Mesh (SX1262)
  bool loraOk = lora_mesh_init();
  if (!loraOk) {
    Serial.println("[WARN] LoRa mesh init FAILED — audio detection active, LoRa offline");
  }
  return loraOk;
}

// ============================================================
//  Hàm khởi tạo
// ============================================================
void setup() {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  
  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

  Serial.begin(115200);
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 2000)) {
    delay(10);
  }
  delay(200);

  // 1. Khởi tạo phần cứng
  i2s_init();
  sd_init();
  initFromExistingFiles();

  // 2. Khởi tạo hệ thống detection (FreeRTOS tasks)
  bool loraOk = startDetectionSystem();
  if (loraOk) {
    digitalWrite(LED, HIGH);
  }

  Serial.println("\n==================================================================");
  Serial.printf("  ESP32-S3 NODE FULL | ID: %d (0x%02X) | %s\n",
                MY_NODE_ID, MY_NODE_ID, loraOk ? "READY" : "LORA FAIL");
  Serial.printf("  MAC: %s | RF: %.1fMHz SF%d BW%.0f +%ddBm\n",
                lora_mesh_getMacString(), LORA_FREQUENCY, LORA_SPREADING_FACTOR,
                LORA_BANDWIDTH, LORA_TX_POWER);
  Serial.printf("  Audio: 16kHz Stereo Dual-Mic (Gain: %.1fx) | Tier0 VAD -> Tier1 KWS\n", FIXED_GAIN);
  Serial.printf("  Batt: %.2fV | Heap: %luKB | PSRAM: %luKB | SD: %s\n",
                lora_mesh_getBatteryMilliVolts() / 1000.0f,
                (unsigned long)(ESP.getFreeHeap() / 1024),
                (unsigned long)(ESP.getFreePsram() / 1024),
                sdReady ? "Mounted" : "Fail");
  Serial.println("  Commands: [t] SOS  [p] Ping  [h] Heartbeat  [m] Routes  [w] Watchdog  [s] Power  [r] Rec WAV");
  Serial.println("==================================================================");
  Serial.println("[SYS] Detection pipeline active — listening for audio events...\n");
}
// ============================================================
//  Vòng lặp chính — xử lý lệnh Serial
//  Detection chạy trên FreeRTOS tasks riêng, loop() chỉ xử lý
//  lệnh người dùng cho chế độ ghi WAV thu data train AI
// ============================================================
static bool continuousMode = false;

/// Tạm dừng detection pipeline để ghi WAV không bị xung đột I2S
static void pauseDetection() {
  i2sCaptureTask_pause();
  Serial.println("[SYS] Detection paused");
}

/// Bật lại detection pipeline sau khi ghi WAV xong
static void resumeDetection() {
  i2sCaptureTask_resume();
  Serial.println("[SYS] Detection resumed");
}

void loop() {

  // Xử lý báo động
  if (g_alarm_end_ms > 0) {
    if (millis() < g_alarm_end_ms) {
      digitalWrite(ALARM_PIN, HIGH);
    } else {
      digitalWrite(ALARM_PIN, LOW);
      g_alarm_end_ms = 0; // Reset
    }
  }

  // Xử lý LoRa mesh (relay + heartbeat)
  lora_mesh_loop();

  static String serialCmdBuf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialCmdBuf.trim();
      if (serialCmdBuf.length() > 0) {
        if (serialCmdBuf.startsWith("CHAT:")) {
          lora_mesh_processChatCommand(serialCmdBuf.c_str());
        } else if (serialCmdBuf.length() == 1) {
          char cmd = serialCmdBuf[0];
          switch (cmd) {
          case 'r':
          case 'R':
            pauseDetection();
            recordWavFile();
            resumeDetection();
            break;
          case 'c':
          case 'C':
            continuousMode = !continuousMode;
            if (continuousMode) pauseDetection();
            else resumeDetection();
            Serial.printf("[SYS] Continuous recording: %s\n", continuousMode ? "ON" : "OFF");
            break;
          case 'q':
          case 'Q':
            continuousMode = false;
            resumeDetection();
            Serial.println("[CMD] Stopped continuous recording");
            break;
          case 't':
          case 'T':
            Serial.println("[CMD] Test SOS -> Gateway (dest: 0)...");
            g_alarm_end_ms = millis() + 7000;
            lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, GATEWAY_ID);
            break;
          case 'p':
          case 'P':
            Serial.println("[CMD] Ping Broadcast -> All Nodes (req ACK)...");
            lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, BROADCAST_ID);
            break;
          case 'h':
          case 'H':
            Serial.println("[CMD] Test Heartbeat Telemetry...");
            lora_mesh_sendHeartbeat(false);
            break;
          case 'm':
          case 'M':
            Serial.println("[CMD] Mesh Routing Table:");
            lora_mesh_printRoutingTable();
            break;
          case 'w':
          case 'W':
            Serial.printf("[CMD] RX Watchdog status: %s (Timeout: %lums) | Resets: %lu\n",
                          LORA_RX_WATCHDOG_ENABLED ? "ACTIVE" : "DISABLED",
                          (unsigned long)LORA_RX_WATCHDOG_TIMEOUT_MS,
                          (unsigned long)lora_mesh_getWatchdogResets());
            break;
          case 's':
          case 'S':
            Serial.printf("[CMD] Power Saving: SX1262 RXPS: %s (Level %d)\n",
                          LORA_RX_DUTY_CYCLE_ENABLED ? "ON" : "OFF",
                          LORA_RXPS_LEVEL);
            break;
          default:
            break;
          }
        }
      }
      serialCmdBuf = "";
    } else {
      if (serialCmdBuf.isEmpty() && c != 'C' && c != 'c') {
        // Phím tắt đơn lẻ không phải tiền tố CHAT -> thực thi ngay lập tức
        switch (c) {
        case 'r':
        case 'R':
          pauseDetection();
          recordWavFile();
          resumeDetection();
          break;
        case 'q':
        case 'Q':
          continuousMode = false;
          resumeDetection();
          Serial.println("[CMD] Stopped continuous recording");
          break;
        case 't':
        case 'T':
          Serial.println("[CMD] Test SOS -> Gateway (dest: 0)...");
          g_alarm_end_ms = millis() + 7000;
          lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, GATEWAY_ID);
          break;
        case 'p':
        case 'P':
          Serial.println("[CMD] Ping Broadcast -> All Nodes (req ACK)...");
          lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, BROADCAST_ID);
          break;
        case 'h':
        case 'H':
          Serial.println("[CMD] Test Heartbeat Telemetry...");
          lora_mesh_sendHeartbeat(false);
          break;
        case 'm':
        case 'M':
          Serial.println("[CMD] Mesh Routing Table:");
          lora_mesh_printRoutingTable();
          break;
        case 'w':
        case 'W':
          Serial.printf("[CMD] RX Watchdog status: %s (Timeout: %lums) | Resets: %lu\n",
                        LORA_RX_WATCHDOG_ENABLED ? "ACTIVE" : "DISABLED",
                        (unsigned long)LORA_RX_WATCHDOG_TIMEOUT_MS,
                        (unsigned long)lora_mesh_getWatchdogResets());
          break;
        case 's':
        case 'S':
          Serial.printf("[CMD] Power Saving: SX1262 RXPS: %s (Level %d)\n",
                        LORA_RX_DUTY_CYCLE_ENABLED ? "ON" : "OFF",
                        LORA_RXPS_LEVEL);
          break;
        default:
          break;
        }
      } else {
        if (serialCmdBuf.length() < 256) {
          serialCmdBuf += c;
        }
      }
    }
  }

  // ============================================================
  //  Telemetry: Tự động gửi Heartbeat pin định kỳ về Gateway
  // ============================================================
  static unsigned long lastHeartbeatTx = 0;
  if (millis() - lastHeartbeatTx >= LORA_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatTx = millis();
    lora_mesh_sendHeartbeat(false);
  }

  if (continuousMode && sdReady) {
    if (!recordWavFile()) {
      continuousMode = false;
      resumeDetection();
      Serial.println("[SYS] Dung ghi lien tuc (loi)");
    }
  }

  delay(10);
}