// ============================================================
//  LORA NODE CẦM TAY — Thiết bị Balo / Hiện trường (Role Node)
//  Kết nối Smartphone qua BLE (Nordic UART Service)
//  Chạy LoRa Mesh + BLE + Chat hai chiều
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"
#include "ble/BleMeshLink.h"

volatile unsigned long g_alarm_end_ms = 0;

static void onChatReceived(uint8_t src_id, uint8_t dest_id, const char* text, uint16_t msg_id, uint8_t hops, float rssi, float snr) {
  // Gửi sự kiện CHAT lên Mobile App qua BLE
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"CHAT\",\"node_id\":\"NODE%u\",\"src\":%u,\"dest\":%u,\"text\":\"%s\",\"msg_id\":%u,\"hops\":%u,\"rssi\":%.1f,\"snr\":%.1f}\n",
           src_id, src_id, dest_id, text, msg_id, hops, rssi, snr);
  bleMeshNotifyLine(String(buf));
}

static void onChatStatus(uint32_t client_id, uint16_t msg_id, const char* status, const char* reason) {
  // Gửi sự kiện CHAT_STATUS lên Mobile App qua BLE
  char buf[192];
  if (reason) {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"CHAT_STATUS\",\"client_id\":%lu,\"msg_id\":%u,\"status\":\"%s\",\"reason\":\"%s\"}\n",
             (unsigned long)client_id, msg_id, status, reason);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"type\":\"CHAT_STATUS\",\"client_id\":%lu,\"msg_id\":%u,\"status\":\"%s\"}\n",
             (unsigned long)client_id, msg_id, status);
  }
  bleMeshNotifyLine(String(buf));
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

  // Khởi tạo BLE Nordic UART Service
  char bleName[32];
  snprintf(bleName, sizeof(bleName), "SOS-Node-%02X", MY_NODE_ID);
  bool bleOk = bleMeshInit(bleName);

  lora_mesh_setChatCallback(onChatReceived);
  lora_mesh_setChatStatusCallback(onChatStatus);

  Serial.println("\n==================================================================");
  Serial.printf("  ESP32-S3 LORA NODE | ID: %d (0x%02X) | %s | BLE: %s\n", 
                MY_NODE_ID, MY_NODE_ID, loraOk ? "READY" : "LORA FAIL",
                bleOk ? bleName : "FAIL");
  Serial.printf("  MAC: %s | RF: %.1fMHz SF%d BW%.0f +%ddBm\n",
                lora_mesh_getMacString(), LORA_FREQUENCY, LORA_SPREADING_FACTOR,
                LORA_BANDWIDTH, LORA_TX_POWER);
  Serial.println("  Interface: BLE Nordic UART + USB Serial");
  Serial.println("  App Command: CHAT:<client_id>:<target_node>:<utf8_text>\\n");
  Serial.println("==================================================================");
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

  // 1. Kiểm tra lệnh nhận từ Mobile App qua BLE
  char bleCmd[128];
  if (bleMeshTakeCommand(bleCmd, sizeof(bleCmd))) {
    Serial.printf("[BLE] Recv: \"%s\"\n", bleCmd);
    if (strncmp(bleCmd, "CHAT:", 5) == 0) {
      lora_mesh_processChatCommand(bleCmd);
    }
  }

  // 2. Kiểm tra lệnh từ Serial console
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

