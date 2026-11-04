// ============================================================
//  LORA GATEWAY — Trạm Cổng Trung Tâm Chỉ Huy (Role Gateway)
//  Chức năng chuyên biệt:
//  1. Thu / phát gói tin qua mạng vô tuyến LoRa Mesh
//  2. Hiển thị log trạng thái và dữ liệu qua UART (Chuẩn NDJSON)
//  3. Gửi / nhận tin nhắn Chat hai chiều với các Node hiện trường
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"

void setup() {
  // 1. Khởi tạo cổng Serial giao tiếp PC (115200 baud)
  Serial.begin(GATEWAY_SERIAL_BAUD);

  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 2000)) {
    delay(10);
  }
  delay(200);

  // 2. Khởi tạo phần cứng LoRa SX1262 với sơ đồ chân riêng của Gateway
  bool loraOk = lora_mesh_init();

  // 3. Xuất log thông tin cấu hình qua UART
  Serial.println("\n==================================================================");
  Serial.printf("  ESP32-S3 LORA GATEWAY | Node ID: %d (0x%02X) | %s\n", 
                MY_NODE_ID, MY_NODE_ID, loraOk ? "READY" : "INIT FAILED");
  Serial.printf("  MAC: %s | RF: %.1fMHz SF%d BW%.0f +%ddBm\n",
                lora_mesh_getMacString(), LORA_FREQUENCY, LORA_SPREADING_FACTOR,
                LORA_BANDWIDTH, LORA_TX_POWER);
  Serial.println("  Interface: PC USB Serial (115200) — Format: NDJSON");
  Serial.println("  PC Command: CHAT:<client_id>:<target_node>:<utf8_text>\\n");
  Serial.println("==================================================================");

  // 4. Phát sự kiện sẵn sàng dạng JSON cho phần mềm PC
  if (loraOk) {
    Serial.println("{\"type\":\"GATEWAY_READY\",\"node_id\":0,\"mac\":\"" + String(lora_mesh_getMacString()) + "\"}\n");
  }
}

static String serialBuffer = "";

void loop() {
  // 1. Luồng tiếp nhận & chuyển tiếp mọi thông tin qua LoRa Mesh (SOS, Chat, Heartbeat, ACK...)
  //    Tự động giải mã và xuất dữ liệu NDJSON ra cổng UART cho phần mềm PC
  lora_mesh_loop();

  // 2. Luồng đọc lệnh gửi tin nhắn từ PC qua UART
  //    Cú pháp: CHAT:<client_id>:<target_node>:<utf8_text>\n
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        if (serialBuffer.startsWith("CHAT:")) {
          lora_mesh_processChatCommand(serialBuffer.c_str());
        }
      }
      serialBuffer = "";
    } else {
      if (serialBuffer.length() < 256) {
        serialBuffer += c;
      }
    }
  }

  delay(2);
}
