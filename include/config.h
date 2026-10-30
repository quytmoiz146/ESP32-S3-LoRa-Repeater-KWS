#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define LED 48
#define ALARM_PIN 9

extern volatile unsigned long g_alarm_end_ms;

// ============================================================
//  Cấu hình chân I2S (Dual Mic ICS-43434)
//  2 mic dùng chung 1 cổng I2S: BCLK, WS, SD
//  Mic L: chân L/R nối GND → phát ở WS mức thấp
//  Mic R: chân L/R nối VDD → phát ở WS mức cao
// ============================================================
#define I2S_BCK_PIN 12     // BCLK  - Xung nhịp bit (chung 2 mic)
#define I2S_WS_PIN 14      // LRCL  - Chọn kênh trái/phải (chung 2 mic)
#define I2S_DATA_IN_PIN 13 // SD    - Dữ liệu ra từ mic (chung 2 mic)

// ============================================================
//  Cấu hình I2S (Stereo — 2 kênh)
// ============================================================
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE 16000 // Tần số lấy mẫu (Hz)
#define I2S_SAMPLE_BITS                                                        \
  I2S_BITS_PER_SAMPLE_32BIT // Đọc 32-bit (native), downscale → 16-bit
#define I2S_CHANNEL I2S_CHANNEL_FMT_RIGHT_LEFT // Stereo: L + R interleaved

// Cấu hình bộ đệm DMA
#define I2S_DMA_BUF_COUNT 8
#define I2S_DMA_BUF_LEN 1024

// Số frame stereo đọc mỗi lần (~32ms @16kHz)
// Mỗi frame = 1 mẫu Left + 1 mẫu Right
#define I2S_FRAMES_PER_READ 512
// Tổng mẫu int32 từ DMA (stereo interleaved: L0,R0,L1,R1,...)
#define I2S_SAMPLES_PER_READ (I2S_FRAMES_PER_READ * 2)

// ============================================================
//  Cấu hình chân thẻ SD (chế độ SPI)
// ============================================================
#define SD_CS_PIN 38   // Chip Select
#define SD_SCK_PIN 40  // Clock
#define SD_MOSI_PIN 39 // MOSI
#define SD_MISO_PIN 41 // MISO

// ============================================================
//  Cấu hình ghi âm WAV (giữ lại để thu data train AI)
// ============================================================
#define WAV_RECORD_DURATION_SEC 1.0f // Thời lượng ghi mỗi file (giây)
#define WAV_FOLDER "/recordings"     // Thư mục lưu tất cả file WAV
#define WAV_FILE_PREFIX "rec"        // Tiền tố tên file (vd: rec_0001.wav)
#define CSV_FILENAME "/recordings/log.csv" // File CSV ghi thông số các bản ghi

// ============================================================
//  Cấu hình Gain — Fixed, không AGC
//  Đã calibrate: peak raw × 6.7 không gây clipping
//  QUAN TRỌNG: giá trị này PHẢI giống hệt giữa recording và live
// ============================================================
#define FIXED_GAIN 6.7f

// ============================================================
//  Cấu hình Tier0 — VAD Energy Gate
// ============================================================
// Ngưỡng bật gate: energy >= noiseFloor * VAD_ON_MULTIPLIER
#define VAD_ON_MULTIPLIER 3.0f
// Ngưỡng tắt gate: energy < noiseFloor * VAD_OFF_MULTIPLIER VÀ hết holdTime
#define VAD_OFF_MULTIPLIER 1.5f
// Giữ gate mở qua khoảng ngắt quãng ngắn (ms)
#define VAD_HOLD_TIME_MS 400
// Sàn năng lượng tối thiểu (tránh noise floor = 0 trong môi trường cực yên)
#define VAD_MIN_NOISE_FLOOR 50.0f
// Thời gian calibration khi khởi động (ms) — đo noise floor ban đầu
#define VAD_CALIBRATION_TIME_MS 2000
// Hệ số EMA (Exponential Moving Average) cập nhật noise floor
#define VAD_EMA_ALPHA 0.01f
// Tỷ lệ EMA chậm khi ACTIVE (slowAlpha = emaAlpha * ratio)
#define VAD_SLOW_EMA_RATIO 0.1f
// Thời gian ACTIVE tối đa trước khi force reset (ms) — chống kẹt vĩnh viễn
#define VAD_MAX_ACTIVE_DURATION_MS 30000

// ============================================================
//  Cấu hình Tier1 — Keyword Spotting (KWS)
// ============================================================
// --- Cửa sổ inference ---
// Kích thước cửa sổ audio cho KWS inference (ms)
#define KWS_WINDOW_SIZE_MS 1000
// Kích thước cửa sổ audio tính bằng số mẫu
#define KWS_WINDOW_SAMPLES (I2S_SAMPLE_RATE * KWS_WINDOW_SIZE_MS / 1000)
// Bước trượt sliding window (ms) — mỗi lần dịch cửa sổ tiến bao nhiêu
// 500ms stride trên 1000ms window = 50% overlap
#define KWS_SLIDE_MS 500
#define KWS_SLIDE_SAMPLES (I2S_SAMPLE_RATE * KWS_SLIDE_MS / 1000)

// --- Ngưỡng margin (bawl - max(noise,speech)) ---
// Ngưỡng bắt đầu theo dõi candidate
// 0.45 = bawl phải chiếm ≥72% confidence (vì margin = bawl - other ≥ 0.45
//         → bawl ≥ 0.45 + other, mà bawl + other ≈ 1 → bawl ≈ 0.725)
// Đủ cao để lọc false positive rải rác (thường margin < 0.3)
// Đủ thấp để không bỏ sót bawl thật (thường margin > 0.6)
#define KWS_MARGIN_THRESHOLD 0.45f
// Thời lượng tối thiểu duy trì margin liên tục để confirm (ms)
// Do đã có luật fast-reject (reset ngay nếu có 1 frame margin < 0),
// ta giảm duration xuống 1000ms (cần khoảng 3 frame liên tiếp) để tránh quá
// điếc.
#define KWS_MIN_DURATION_MS 0

// Cooldown sau khi confirmed — tránh trigger liên tục (ms)
#define KWS_COOLDOWN_MS 5000

// ============================================================
//  Cấu hình Ring Buffer (PSRAM)
// ============================================================
// Kích thước ring buffer tính bằng số mẫu int16_t (mono, sau downmix)
// Mặc định: 2 giây = 32000 mẫu @16kHz = 64KB
#define RING_BUFFER_SIZE_SAMPLES (I2S_SAMPLE_RATE * 2)

// ============================================================
//  Cấu hình Dual Mic — Phân tích chênh lệch kênh
// ============================================================
// Ngưỡng chênh lệch energy L/R: max(L,R)/min(L,R) > ratio → bất thường
#define MIC_IMBALANCE_RATIO 5.0f
// Số khối liên tiếp vượt ngưỡng để báo cảnh báo mic hỏng (~100 × 32ms ≈ 3.2s)
#define MIC_IMBALANCE_COUNT_LIMIT 100
// Ngưỡng energy tối thiểu để tính ILD (tránh chia cho số nhỏ khi yên lặng)
#define MIC_MIN_ENERGY_FOR_ILD 10.0f

// ============================================================
//  Cấu hình FreeRTOS Tasks
// ============================================================
// taskI2SCapture — Core 0, priority cao (realtime audio)
// Stack tăng từ 8192 lên 12288 vì buffer stereo lớn hơn
#define TASK_I2S_CAPTURE_STACK 12288
#define TASK_I2S_CAPTURE_PRIORITY 5
#define TASK_I2S_CAPTURE_CORE 0

// taskKeywordSpot — Core 1, priority trung bình
#define TASK_KWS_STACK 16384
#define TASK_KWS_PRIORITY 3
#define TASK_KWS_CORE 1

// ============================================================
//  CSV log cho detection events
// ============================================================
#define DETECTION_CSV_FILENAME "/recordings/detection_log.csv"

// ============================================================
//  Cấu hình LoRa SX1262 (Ai-Thinker Ra-01SH)
//  Module này dùng SPI riêng (HSPI), không xung đột với SD card
// ============================================================
// --- Chân SPI cho LoRa ---
#define LORA_SCK_PIN 7
#define LORA_MOSI_PIN 5
#define LORA_MISO_PIN 6
#define LORA_CS_PIN 4

// --- Chân điều khiển ---
#define LORA_RST_PIN 8
#define LORA_BUSY_PIN 16
#define LORA_DIO1_PIN 20

// --- Chân RF Switch (điều khiển anten thu/phát) ---
// Tạm tắt chân ngoài vì module Ra-01SH dùng chân DIO2 bên trong chip
#define LORA_RXEN_PIN -1
#define LORA_TXEN_PIN -1

// --- TCXO voltage (0.0 nếu không dùng TCXO, 1.8 cho một số module) ---
#define LORA_TCXO_VOLTAGE 0.0f

// --- RF Parameters ---
#define LORA_FREQUENCY        921.4f
#define LORA_BANDWIDTH        125.0f
#define LORA_SPREADING_FACTOR 9
#define LORA_CODING_RATE      7
#define LORA_TX_POWER         22

// ID mặc định của các loại thiết bị trong mạng mesh
#define GATEWAY_ID   0
#define BROADCAST_ID 255

// Khai báo hàm lấy ID và MAC eFuse phần cứng từ module lora_mesh
uint8_t lora_mesh_getNodeId();
const char* lora_mesh_getMacString();

// ID của thiết bị này trong mạng mesh: Tự động lấy từ byte cuối của MAC eFuse ESP32
#define MY_NODE_ID (lora_mesh_getNodeId())

// ============================================================
//  Cấu hình Tiết kiệm năng lượng (Power Saving - MeshCore style)
// ============================================================
// --- LoRa Preamble (16 symbols chuẩn hóa cho SF9 và RX Duty Cycle) ---
#define LORA_PREAMBLE_LEN 16

// --- Chế độ thu chu kỳ động (Dynamic RX Duty Cycle / RXPS) ---
// Chế độ tiết kiệm năng lượng kép (Tầng 1 - RF): SX1262 Dynamic RX Duty Cycle (MeshCore style)
#define LORA_RX_DUTY_CYCLE_ENABLED 1
#define RXPS_LEVEL_CONSERVATIVE 3
#define RXPS_LEVEL_BALANCED     6
#define RXPS_LEVEL_MAX          8
#define LORA_RXPS_LEVEL         RXPS_LEVEL_BALANCED

// Giá trị mặc định dự phòng nếu tính toán gặp lỗi
#define LORA_RX_DUTY_CYCLE_RX_US 40000UL    // 40ms nghe
#define LORA_RX_DUTY_CYCLE_SLEEP_US 25000UL // 25ms ngủ

// --- Kiểm tra kênh tránh va chạm (CAD - Channel Activity Detection) ---
#define LORA_CAD_ENABLED 1
#define LORA_CAD_MAX_RETRIES 3

// --- RX Watchdog (RXPS Watchdog - MeshCore style) ---
// Tự động kiểm tra kẹt BUSY, lỗi SPI hoặc refresh chu kỳ RXPS nếu radio im lặng quá lâu
#define LORA_RX_WATCHDOG_ENABLED    1
#define LORA_RX_WATCHDOG_TIMEOUT_MS 45000UL // 45 giây kiểm tra & khôi phục radio

// --- Định tuyến Mesh: Kích hoạt Unicast Next-Hop ---
// Khi đã học được tuyến đường, gửi đích danh qua next_hop thay vì flood broadcast
#define LORA_UNICAST_ROUTING_ENABLED 1

// --- Quản lý giấc ngủ MCU (Light Sleep cho ESP32 Repeater) ---
// Chế độ tiết kiệm năng lượng kép (Tầng 2 - MCU): Light Sleep khi rảnh rỗi, thức dậy bằng DIO1 (<0.5ms) hoặc Timer
#define LORA_LIGHT_SLEEP_ENABLED 1
#define LORA_LIGHT_SLEEP_PERIOD_MS 5000 // Tự động thức sau 5s để chạy tác vụ nền & kiểm tra lệnh Serial

// --- QoS: Cơ chế Xác nhận (ACK) & Gửi lại (Retries) ---
#define LORA_QOS_ACK_TIMEOUT_MS 1800 // Timeout đợi ACK (ms)
#define LORA_QOS_MAX_RETRIES    3    // Số lần phát lại tối đa

// --- Giám sát Pin & Bảo vệ Sụt nguồn (Battery Protection) ---
#define PIN_VBAT_READ           1     // Chân ADC đọc pin (GPIO1 - ADC1 CH0 trên ESP32-S3)
#define BATTERY_DIVIDER_RATIO   2.0f  // Hệ số cầu phân áp (100k : 100k -> x2)
#define BATTERY_CRITICAL_MV     3000  // Ngưỡng kiệt pin khẩn cấp (3.0V -> Deep Sleep bảo vệ)

// --- Telemetry & Giám sát Định kỳ (Periodic Heartbeat) ---
// Mặc định gửi điện áp pin và trạng thái node mỗi 15 phút (900.000 ms)
// Đặt giá trị nhỏ hơn (vd: 30000ms = 30s) nếu muốn kiểm tra nhanh khi test
#define LORA_HEARTBEAT_INTERVAL_MS (15 * 60 * 1000UL)

#endif // CONFIG_H