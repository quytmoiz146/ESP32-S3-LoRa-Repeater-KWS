// ============================================================
//  LORA GATEWAY — Trạm Cổng Trung Tâm Chỉ Huy (Role Gateway)
//  Chức năng chuyên biệt:
//  1. Thu / phát gói tin qua mạng vô tuyến LoRa Mesh
//  2. Hiển thị log trạng thái và dữ liệu qua UART (Chuẩn NDJSON)
//  3. Gửi / nhận tin nhắn Chat hai chiều với các Node hiện trường
//  4. Gửi lệnh điều khiển bật / tắt còi báo động tại Repeater qua LoRa
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"

void setup() {
  // 1. Khởi tạo cổng Serial giao tiếp PC (115200 baud)
  Serial.begin(GATEWAY_SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0); // Không block nếu PC chưa kịp mở Serial Monitor
#endif

  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 2000)) {
    delay(10);
  }
  delay(200);

  // 2. Khởi tạo phần cứng LoRa SX1262 với sơ đồ chân riêng của Gateway
  bool loraOk = lora_mesh_init();

  // 3. Phát sự kiện sẵn sàng chuẩn NDJSON cho dashboard / máy tính PC (không in banner văn bản)
  if (loraOk) {
    Serial.printf("{\"type\":\"GATEWAY_READY\",\"node_id\":%d,\"mac\":\"%s\",\"rf_mhz\":%.1f,\"sf\":%d,\"bw\":%.0f,\"tx_power\":%d}\n",
                  MY_NODE_ID, lora_mesh_getMacString(), LORA_FREQUENCY,
                  LORA_SPREADING_FACTOR, LORA_BANDWIDTH, LORA_TX_POWER);
  } else {
    Serial.println("{\"type\":\"GATEWAY_ERROR\",\"error\":\"LORA_INIT_FAILED\"}");
  }
}

static String serialBuffer = "";

void loop() {
  // 1. Luồng tiếp nhận & chuyển tiếp mọi thông tin qua LoRa Mesh (SOS, Chat, ACK...)
  //    Tự động giải mã và xuất dữ liệu NDJSON ra cổng UART cho phần mềm PC
  lora_mesh_loop();

  // 2. Luồng đọc lệnh từ PC qua UART (Chuẩn NDJSON / Text Command)
  //    Hỗ trợ:
  //      - CHAT:<client_id>:<target_node>:<utf8_text>\n
  //      - ALARM:ALL:ON\n / ALARM:ALL:OFF\n
  //      - ALARM:<id>:ON\n / ALARM:<id>:OFF\n
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        if (serialBuffer.startsWith("CHAT:")) {
          lora_mesh_processChatCommand(serialBuffer.c_str());
        } else if (serialBuffer.startsWith("ALARM:")) {
          lora_mesh_processAlarmCommand(serialBuffer.c_str());
        } else {
          Serial.println("{\"type\":\"CMD_STATUS\",\"status\":\"FAILED\",\"reason\":\"UNKNOWN_COMMAND\"}");
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
