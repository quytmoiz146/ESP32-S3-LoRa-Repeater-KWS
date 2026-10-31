// ============================================================
//  LORA GATEWAY — Trạm cổng trung tâm chỉ huy (Role Gateway)
//  Cắm USB vào máy tính, giao tiếp qua Serial NDJSON (115200 baud)
//  Không cần BLE — Thuần LoRa Mesh + USB Serial
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"

volatile unsigned long g_alarm_end_ms = 0;

static void onChatReceived(uint8_t src_id, uint8_t dest_id, const char* text, uint16_t msg_id, uint8_t hops, float rssi, float snr) {
  // Đã được lora_mesh tự động format NDJSON ra Serial
  // Có thể gắn thêm còi hoặc xử lý giao diện phụ tại đây nếu cần
}

static void onChatStatus(uint32_t client_id, uint16_t msg_id, const char* status, const char* reason) {
  // Đã được lora_mesh tự động format NDJSON CHAT_STATUS ra Serial
}

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

  bool loraOk = lora_mesh_init();
  if (loraOk) {
    digitalWrite(LED, HIGH);
  }

  lora_mesh_setChatCallback(onChatReceived);
  lora_mesh_setChatStatusCallback(onChatStatus);

  Serial.println("\n==================================================================");
  Serial.printf("  ESP32-S3 LORA GATEWAY | Node ID: %d (0x%02X) | %s\n", 
                MY_NODE_ID, MY_NODE_ID, loraOk ? "READY" : "INIT FAILED");
  Serial.printf("  MAC: %s | RF: %.1fMHz SF%d BW%.0f +%ddBm\n",
                lora_mesh_getMacString(), LORA_FREQUENCY, LORA_SPREADING_FACTOR,
                LORA_BANDWIDTH, LORA_TX_POWER);
  Serial.println("  Interface: PC USB Serial (115200) — Format: NDJSON");
  Serial.println("  PC Command: CHAT:<client_id>:<target_node>:<utf8_text>\\n");
  Serial.println("  Test Keys : [t] SOS  [p] Ping  [h] Heartbeat  [r] Routes  [w] Watchdog");
  Serial.println("==================================================================");
  if (loraOk) {
    Serial.println("{\"type\":\"GATEWAY_READY\",\"node_id\":0,\"mac\":\"" + String(lora_mesh_getMacString()) + "\"}\n");
  }
}

static String serialBuffer = "";

void loop() {
  // Xử lý báo động
  if (g_alarm_end_ms > 0) {
    if (millis() < g_alarm_end_ms) {
      digitalWrite(ALARM_PIN, HIGH);
    } else {
      digitalWrite(ALARM_PIN, LOW);
      g_alarm_end_ms = 0;
    }
  }

  lora_mesh_loop();

  // Đọc lệnh từ Serial (hỗ trợ cả chuỗi CHAT:... và các phím tắt đơn lẻ)
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        if (serialBuffer.startsWith("CHAT:")) {
          lora_mesh_processChatCommand(serialBuffer.c_str());
        } else if (serialBuffer.length() == 1) {
          char cmd = serialBuffer[0];
          if (cmd == 't' || cmd == 'T') {
            Serial.println("[CMD] Test SOS Broadcast...");
            lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, BROADCAST_ID);
          } else if (cmd == 'p' || cmd == 'P') {
            Serial.println("[CMD] Ping Broadcast -> All Nodes (req ACK)...");
            lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true, BROADCAST_ID);
          } else if (cmd == 'h' || cmd == 'H') {
            Serial.println("[CMD] Test Heartbeat...");
            lora_mesh_sendHeartbeat(false);
          } else if (cmd == 'r' || cmd == 'R') {
            Serial.println("[CMD] Mesh Routing Table:");
            lora_mesh_printRoutingTable();
          } else if (cmd == 'w' || cmd == 'W') {
            Serial.printf("[CMD] RX Watchdog status: Resets: %lu\n",
                          (unsigned long)lora_mesh_getWatchdogResets());
          }
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

