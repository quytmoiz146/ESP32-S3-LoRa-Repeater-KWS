#ifndef CONFIG_COMMON_H
#define CONFIG_COMMON_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
//  THAM SỐ DÙNG CHUNG TOÀN HỆ THỐNG LORA MESH
//  (Đồng bộ trên cả 3 thiết bị: Repeater, Node, Gateway)
// ============================================================

// --- Biến báo động toàn cục (chỉ kích hoạt trên thiết bị có còi phần cứng) ---
#if defined(ALARM_PIN) && (ALARM_PIN >= 0)
extern volatile unsigned long g_alarm_end_ms;
#endif

// --- Tham số sóng vô tuyến LoRa (SX1262 RF Parameters) ---
#define LORA_FREQUENCY        921.4f   // Tần số hoạt động (MHz)
#define LORA_BANDWIDTH        125.0f   // Băng thông (kHz)
#define LORA_SPREADING_FACTOR 9        // Hệ số trải phổ SF9 (cân bằng tầm xa và tốc độ)
#define LORA_CODING_RATE      7        // Tỷ lệ mã hóa 4/7 (CR=7 trong RadioLib)
#define LORA_TX_POWER         22       // Công suất phát cực đại +22 dBm
#define LORA_PREAMBLE_LEN     16       // Độ dài tiền tố Preamble (16 symbols)

#ifndef LORA_TCXO_VOLTAGE
#define LORA_TCXO_VOLTAGE     0.0f     // Mặc định XTAL nếu không có TCXO riêng
#endif

// --- Địa chỉ mạng định danh ---
#define GATEWAY_ID            0        // ID trạm chỉ huy trung tâm
#define BROADCAST_ID          255      // Địa chỉ phát quảng bá toàn mạng

// --- Hàm lấy ID và địa chỉ MAC eFuse phần cứng ---
uint8_t lora_mesh_getNodeId();
const char* lora_mesh_getMacString();

// ID của thiết bị tự động lấy từ byte cuối của MAC eFuse ESP32 (trừ khi được cấu hình cố định như Gateway)
#ifndef MY_NODE_ID
#define MY_NODE_ID (lora_mesh_getNodeId())
#endif

// --- Kiểm tra kênh tránh va chạm (CAD - Channel Activity Detection) ---
#define LORA_CAD_ENABLED      1
#define LORA_CAD_MAX_RETRIES  3

// --- RX Watchdog: Tự động khôi phục chip SX1262 khi bị kẹt ---
#define LORA_RX_WATCHDOG_ENABLED    1
#define LORA_RX_WATCHDOG_TIMEOUT_MS 45000UL // 45 giây kiểm tra & khôi phục radio

// --- Định tuyến Mesh: Kích hoạt Unicast Next-Hop ---
#define LORA_UNICAST_ROUTING_ENABLED 1

// --- QoS: Cơ chế Xác nhận (ACK) & Gửi lại (Retries) ---
#define LORA_QOS_ACK_TIMEOUT_MS 1800   // Timeout chờ ACK (ms)
#define LORA_QOS_MAX_RETRIES    3      // Số lần phát lại tối đa

// --- Telemetry: Chu kỳ gửi Heartbeat định kỳ ---
#ifndef LORA_HEARTBEAT_INTERVAL_MS
#define LORA_HEARTBEAT_INTERVAL_MS (15 * 60 * 1000UL) // 15 phút
#endif

// ============================================================
//  Các hằng số & Giá trị dự phòng (Defaults Fallback)
// ============================================================
#define RXPS_LEVEL_CONSERVATIVE     3
#define RXPS_LEVEL_BALANCED         6
#define RXPS_LEVEL_MAX              8

#ifndef LORA_RX_DUTY_CYCLE_ENABLED
#define LORA_RX_DUTY_CYCLE_ENABLED  0
#endif

#ifndef LORA_RXPS_LEVEL
#define LORA_RXPS_LEVEL             RXPS_LEVEL_BALANCED
#endif

#ifndef LORA_RX_DUTY_CYCLE_RX_US
#define LORA_RX_DUTY_CYCLE_RX_US    40000UL
#endif

#ifndef LORA_RX_DUTY_CYCLE_SLEEP_US
#define LORA_RX_DUTY_CYCLE_SLEEP_US 25000UL
#endif

#ifndef LORA_LIGHT_SLEEP_ENABLED
#define LORA_LIGHT_SLEEP_ENABLED    0
#endif

#ifndef LORA_LIGHT_SLEEP_PERIOD_MS
#define LORA_LIGHT_SLEEP_PERIOD_MS  5000
#endif

#ifndef BATTERY_CRITICAL_MV
#define BATTERY_CRITICAL_MV         3000
#endif

#ifndef BATTERY_DIVIDER_RATIO
#define BATTERY_DIVIDER_RATIO       2.0f
#endif

#endif // CONFIG_COMMON_H
