// ============================================================
//  LORA REPEATER — Trạm chuyển tiếp rẻ tiền (chỉ relay)
//  Không có KWS, I2S, SD card — chỉ nhận và phát lại gói tin
// ============================================================
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include "lora_mesh/lora_mesh.h"

volatile unsigned long g_alarm_end_ms = 0;

void setup() {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

  Serial.begin(115200);

  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }
  delay(500);

  Serial.println("=========================================");
  Serial.printf("  LORA REPEATER — ID=%d (0x%02X)\n", MY_NODE_ID, MY_NODE_ID);
  Serial.printf("  eFuse MAC: %s\n", lora_mesh_getMacString());
  Serial.println("  Relay-only mode (no KWS/Audio/SD)");
  Serial.println("=========================================");

  Serial.printf("[SYS] Free heap: %lu bytes\n",
                (unsigned long)ESP.getFreeHeap());

  if (lora_mesh_init()) {
    Serial.println("[LORA] ✓ INIT SUCCESS!");
    digitalWrite(LED, HIGH);
  } else {
    Serial.println("[LORA] ✗ INIT FAILED!");
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED, HIGH);
      delay(100);
      digitalWrite(LED, LOW);
      delay(100);
    }
  }

  Serial.println("=========================================");
  Serial.println("[SYS] Repeater running — listening...");
  Serial.println("[SYS] Lenh Serial:");
  Serial.println("  t = Gui TEST SOS toi Gateway (dest: 0, relay test)");
  Serial.println("  p = Gui PING BROADCAST (kiem tra ACK 2 chieu tu cac node)");
  Serial.println("  h = Gui TEST Heartbeat pin");
  Serial.println("=========================================");
}

void loop() {
  // Xử lý báo động
  if (g_alarm_end_ms > 0) {
    if (millis() < g_alarm_end_ms) {
      digitalWrite(ALARM_PIN, HIGH);
    } else {
      digitalWrite(ALARM_PIN, LOW);
      g_alarm_end_ms = 0; // Reset
    }
  }

  lora_mesh_loop();

  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 't' || cmd == 'T') {
      Serial.println("[SYS] Gửi TEST SOS BINARY tới Gateway (dest: 0)...");
      g_alarm_end_ms = millis() + 7000;
      lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true /* requireAck */, GATEWAY_ID);
    } else if (cmd == 'p' || cmd == 'P') {
      Serial.println("[SYS] Gửi PING BROADCAST tới toàn mạng với yêu cầu ACK...");
      lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true /* requireAck */, BROADCAST_ID);
    } else if (cmd == 'h' || cmd == 'H') {
      Serial.println("[SYS] Gửi TEST HEARTBEAT (Telemetry Pin)...");
      lora_mesh_sendHeartbeat(false);
    } else if (cmd == 'j' || cmd == 'J') {
      Serial.println("[SYS] Gửi TEST SOS JSON cũ...");
      g_alarm_end_ms = millis() + 7000;
      const char* testPayload = "{\"type\":\"SOS\",\"target\":\"gateway\",\"label\":\"bawl\",\"score\":99.9,\"dir\":\"TEST_REP\"}";
      lora_mesh_sendSOS(testPayload, strlen(testPayload), true /* requireAck */);
    }
  }

  // ============================================================
  //  Telemetry: Tự động gửi Heartbeat pin định kỳ về Gateway
  // ============================================================
  static unsigned long lastHeartbeatTx = 0;
  if (millis() - lastHeartbeatTx >= LORA_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatTx = millis();
    lora_mesh_sendHeartbeat(false);
  }

  // ============================================================
  //  Power Saving: Light Sleep khi rảnh rỗi (MeshCore standard)
  //  Không ngủ nếu đang hú còi hoặc có ký tự trên Serial
  // ============================================================
#if LORA_LIGHT_SLEEP_ENABLED
  if (g_alarm_end_ms == 0 && !Serial.available()) {
    // Ngủ mức Light Sleep (MCU ~1.5 - 2mA). Thức dậy tức thì khi DIO1 lên HIGH (gói tin tới)
    // hoặc định kỳ sau LORA_LIGHT_SLEEP_PERIOD_MS để kiểm tra hệ thống
    lora_mesh_light_sleep(LORA_LIGHT_SLEEP_PERIOD_MS);
  } else {
    delay(10);
  }
#else
  delay(10);
#endif
}