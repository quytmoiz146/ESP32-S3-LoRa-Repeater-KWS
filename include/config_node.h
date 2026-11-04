#ifndef CONFIG_NODE_H
#define CONFIG_NODE_H

#include <stdint.h>

// ============================================================
//  CẤU HÌNH DÀNH RIÊNG CHO THIẾT BỊ NODE CẦM TAY / BALO
//  Chức năng chuyên biệt:
//  1. Kết nối Smartphone qua Bluetooth Low Energy (BLE NUS)
//  2. Nhắn tin hai chiều (Chat) với Gateway và các Node khác qua LoRa Mesh
// ============================================================

// --- Sơ đồ chân Module LoRa SX1262 trên Node (Đồng bộ với Gateway) ---
#define LORA_SCK_PIN    40
#define LORA_MOSI_PIN   39
#define LORA_MISO_PIN   41
#define LORA_CS_PIN     42
#define LORA_RST_PIN    7
#define LORA_BUSY_PIN   48
#define LORA_DIO1_PIN   2
#define LORA_DIO3_PIN   1

// --- Cấu hình BLE Nordic UART Service (NUS) kết nối Mobile App ---
#define BLE_DEVICE_NAME_PREFIX "SOS-Node-"
#define BLE_NUS_SERVICE_UUID   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_NUS_RX_UUID        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_NUS_TX_UUID        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#endif // CONFIG_NODE_H
