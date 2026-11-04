#ifndef CONFIG_REPEATER_H
#define CONFIG_REPEATER_H

#include <stdint.h>
#include <driver/i2s.h>

// ============================================================
//  CẤU HÌNH DÀNH RIÊNG CHO TRẠM LẶP (REPEATER)
//  (Áp dụng cho repeater_full và repeater_simple)
// ============================================================

// --- Ngoại vi cơ bản ---
#define LED             48
#define ALARM_PIN       9

// ============================================================
//  Cấu hình chân I2S (Dual Mic ICS-43434)
//  2 mic dùng chung 1 cổng I2S: BCLK, WS, SD
//  Mic L: chân L/R nối GND -> phát ở WS mức thấp
//  Mic R: chân L/R nối VDD -> phát ở WS mức cao
// ============================================================
#define I2S_BCK_PIN     12     // BCLK  - Xung nhịp bit
#define I2S_WS_PIN      14     // LRCL  - Chọn kênh trái/phải
#define I2S_DATA_IN_PIN 13     // SD    - Dữ liệu ra từ mic

#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 16000  // 16 kHz
#define I2S_SAMPLE_BITS I2S_BITS_PER_SAMPLE_32BIT // 32-bit native -> 16-bit PCM
#define I2S_CHANNEL     I2S_CHANNEL_FMT_RIGHT_LEFT

#define I2S_DMA_BUF_COUNT    8
#define I2S_DMA_BUF_LEN      1024
#define I2S_FRAMES_PER_READ  512
#define I2S_SAMPLES_PER_READ (I2S_FRAMES_PER_READ * 2)

// ============================================================
//  Cấu hình chân thẻ SD (chế độ SPI)
// ============================================================
#define SD_CS_PIN       38
#define SD_SCK_PIN      40
#define SD_MOSI_PIN     39
#define SD_MISO_PIN     41

#define WAV_RECORD_DURATION_SEC 1.0f
#define WAV_FOLDER              "/recordings"
#define WAV_FILE_PREFIX         "rec"
#define CSV_FILENAME            "/recordings/log.csv"
#define DETECTION_CSV_FILENAME  "/recordings/detection_log.csv"

// ============================================================
//  Hệ số Gain & Pipeline Xử Lý Âm Thanh
// ============================================================
#define FIXED_GAIN 6.7f        // Gain cố định (không AGC)

// --- Tier-0: VAD Energy Gate ---
#define VAD_ON_MULTIPLIER       3.0f
#define VAD_OFF_MULTIPLIER      1.5f
#define VAD_HOLD_TIME_MS        400
#define VAD_MIN_NOISE_FLOOR     50.0f
#define VAD_CALIBRATION_TIME_MS 2000
#define VAD_EMA_ALPHA           0.01f
#define VAD_SLOW_EMA_RATIO      0.1f
#define VAD_MAX_ACTIVE_DURATION_MS 30000

// --- Tier-1: Keyword Spotting (KWS) ---
#define KWS_WINDOW_SIZE_MS      1000
#define KWS_WINDOW_SAMPLES      (I2S_SAMPLE_RATE * KWS_WINDOW_SIZE_MS / 1000)
#define KWS_SLIDE_MS            500
#define KWS_SLIDE_SAMPLES       (I2S_SAMPLE_RATE * KWS_SLIDE_MS / 1000)
#define KWS_MARGIN_THRESHOLD    0.45f
#define KWS_MIN_DURATION_MS     0
#define KWS_COOLDOWN_MS         5000

// --- Ring Buffer PSRAM ---
#define RING_BUFFER_SIZE_SAMPLES (I2S_SAMPLE_RATE * 2) // 2 giây audio mono

// --- Dual Mic: Phân tích chênh lệch kênh ILD & Sức khỏe Mic ---
#define MIC_IMBALANCE_RATIO       5.0f
#define MIC_IMBALANCE_COUNT_LIMIT 100
#define MIC_MIN_ENERGY_FOR_ILD    10.0f

// --- Cấu hình FreeRTOS Tasks ---
#define TASK_I2S_CAPTURE_STACK    12288
#define TASK_I2S_CAPTURE_PRIORITY 5
#define TASK_I2S_CAPTURE_CORE     0

#define TASK_KWS_STACK            16384
#define TASK_KWS_PRIORITY         3
#define TASK_KWS_CORE             1

// ============================================================
//  Cấu hình chân LoRa SX1262 trên Trạm Repeater
// ============================================================
#define LORA_SCK_PIN    7
#define LORA_MOSI_PIN   5
#define LORA_MISO_PIN   6
#define LORA_CS_PIN     4
#define LORA_RST_PIN    8
#define LORA_BUSY_PIN   16
#define LORA_DIO1_PIN   20
#define LORA_DIO3_PIN   -1

#define LORA_RXEN_PIN   -1
#define LORA_TXEN_PIN   -1
#define LORA_TCXO_VOLTAGE 0.0f

// ============================================================
//  Chế độ Tiết kiệm năng lượng kép (Dual Power Saving - MeshCore)
// ============================================================
// --- Tầng 1: SX1262 Dynamic RX Duty Cycle (RXPS) ---
#define LORA_RX_DUTY_CYCLE_ENABLED  1
#define RXPS_LEVEL_CONSERVATIVE     3
#define RXPS_LEVEL_BALANCED         6
#define RXPS_LEVEL_MAX              8
#define LORA_RXPS_LEVEL             RXPS_LEVEL_BALANCED

#define LORA_RX_DUTY_CYCLE_RX_US    40000UL // 40ms nghe
#define LORA_RX_DUTY_CYCLE_SLEEP_US 25000UL // 25ms ngủ

// --- Tầng 2: MCU Light Sleep khi rảnh rỗi ---
#define LORA_LIGHT_SLEEP_ENABLED    1
#define LORA_LIGHT_SLEEP_PERIOD_MS  5000    // Tự động thức sau 5s

// ============================================================
//  Giám sát Pin & Bảo vệ Sụt nguồn (Battery Protection)
// ============================================================
#define PIN_VBAT_READ         1     // Chân ADC đọc pin (GPIO1)
#define BATTERY_DIVIDER_RATIO 2.0f  // Cầu phân áp 100k : 100k -> x2
#define BATTERY_CRITICAL_MV   3000  // Ngưỡng kiệt pin khẩn cấp 3.0V

#endif // CONFIG_REPEATER_H

