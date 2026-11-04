#ifndef CONFIG_GATEWAY_H
#define CONFIG_GATEWAY_H

#include <stdint.h>

// ============================================================
//  CẤU HÌNH DÀNH RIÊNG CHO TRẠM CỔNG CHỈ HUY (GATEWAY)
//  Chức năng chuyên biệt:
//  1. Thu / phát gói tin qua mạng vô tuyến LoRa Mesh
//  2. Hiển thị log trạng thái và dữ liệu qua UART (Chuẩn NDJSON)
//  3. Gửi / nhận tin nhắn Chat hai chiều với các Node hiện trường
// ============================================================

// --- Định danh Gateway cố định trong mạng Mesh ---
#define GATEWAY_ID          0
#define MY_NODE_ID          GATEWAY_ID // Gateway luôn có ID = 0

// --- Sơ đồ chân Module LoRa SX1262 trên Gateway ---
#define LORA_SCK_PIN        40
#define LORA_MOSI_PIN       39
#define LORA_MISO_PIN       41
#define LORA_CS_PIN         42
#define LORA_RST_PIN        7
#define LORA_BUSY_PIN       48
#define LORA_DIO1_PIN       2
#define LORA_DIO3_PIN       1

// --- Giao diện UART kết nối Máy Tính (NDJSON) ---
#define GATEWAY_SERIAL_BAUD 115200

#endif // CONFIG_GATEWAY_H
