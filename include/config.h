#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  HỆ THỐNG CẤU HÌNH ĐA VAI TRÒ (MULTI-ROLE CONFIGURATION)
// ============================================================
//  Hệ thống tách bạch cấu hình thành các file riêng cho 3 loại thiết bị:
//  - config_gateway.h  : Sơ đồ chân LoRa SX1262 và NDJSON trạm Gateway
//  - config_node.h     : Sơ đồ chân LoRa SX1262 và BLE NUS thiết bị Node di động
//  - config_repeater.h : Sơ đồ chân I2S Mic, SD, KWS AI, Power Saving trạm Repeater
//  - config_common.h   : Tham số vô tuyến LoRa, địa chỉ mạng, QoS & mặc định
// ============================================================

// 1. Tự động nạp cấu hình phần cứng riêng theo cờ định nghĩa vai trò
#if defined(ROLE_GATEWAY)
  #include "config_gateway.h"
#elif defined(ROLE_NODE)
  #include "config_node.h"
#else
  // Mặc định cho trạm lặp Repeater
  #include "config_repeater.h"
#endif

// 2. Nạp tham số dùng chung toàn hệ thống và các giá trị dự phòng
#include "config_common.h"

#endif // CONFIG_H
