// ============================================================
//  LORA REPEATER — Trạm chuyển tiếp rẻ tiền (chỉ relay)
//  Không có KWS, I2S, SD card — chỉ nhận và phát lại gói tin
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"

volatile unsigned long g_alarm_end_ms = 0;

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

  Serial.println("\n==================================================================");
  Serial.printf("  ESP32-S3 LORA REPEATER | Node ID: %d (0x%02X) | %s\n", 
                MY_NODE_ID, MY_NODE_ID, loraOk ? "READY" : "INIT FAILED");
  Serial.printf("  MAC: %s | RF: %.1fMHz SF%d BW%.0f +%ddBm\n",
                lora_mesh_getMacString(), LORA_FREQUENCY, LORA_SPREADING_FACTOR,
                LORA_BANDWIDTH, LORA_TX_POWER);
  Serial.printf("  Batt: %.2fV | Heap: %luKB | CAD: %s | Dual Power Saving: %s (RXPS:%s, Sleep:%s)\n",
                lora_mesh_getBatteryMilliVolts() / 1000.0f,
                (unsigned long)(ESP.getFreeHeap() / 1024),
                LORA_CAD_ENABLED ? "ON" : "OFF",
                (LORA_RX_DUTY_CYCLE_ENABLED && LORA_LIGHT_SLEEP_ENABLED) ? "ON" : "OFF",
                LORA_RX_DUTY_CYCLE_ENABLED ? "ON" : "OFF",
                LORA_LIGHT_SLEEP_ENABLED ? "ON" : "OFF");
  Serial.println("  Commands: [t] SOS Gateway  [p] Ping Broadcast  [h] Heartbeat  [r] Routes  [w] Watchdog  [s] Power");
  Serial.println("==================================================================");
  if (loraOk) {
    Serial.println("[SYS] Repeater active — listening for mesh packets...\n");
  } else {
    Serial.println("[ERR] LoRa hardware not responding — check SPI wiring!\n");
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED, HIGH);
      delay(100);
      digitalWrite(LED, LOW);
      delay(100);
    }
  }
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
      Serial.println("[CMD] Test SOS -> Gateway (dest: 0)...");
      g_alarm_end_ms = millis() + 7000;
      lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true /* requireAck */, GATEWAY_ID);
    } else if (cmd == 'p' || cmd == 'P') {
      Serial.println("[CMD] Ping Broadcast -> All Nodes (req ACK)...");
      lora_mesh_sendSosBinary(ALERT_TEST, 99, DIR_CENTER, true /* requireAck */, BROADCAST_ID);
    } else if (cmd == 'h' || cmd == 'H') {
      Serial.println("[CMD] Test Heartbeat Telemetry...");
      lora_mesh_sendHeartbeat(false);
    } else if (cmd == 'r' || cmd == 'R') {
      Serial.println("[CMD] Mesh Routing Table:");
      lora_mesh_printRoutingTable();
    } else if (cmd == 'w' || cmd == 'W') {
      Serial.printf("[CMD] RX Watchdog status: %s (Timeout: %lums) | Resets: %lu\n",
                    LORA_RX_WATCHDOG_ENABLED ? "ACTIVE" : "DISABLED",
                    (unsigned long)LORA_RX_WATCHDOG_TIMEOUT_MS,
                    (unsigned long)lora_mesh_getWatchdogResets());
    } else if (cmd == 's' || cmd == 'S') {
      Serial.printf("[CMD] Power Saving: DUAL %s | RXPS (SX1262): %s (Level %d) | Light Sleep (MCU): %s (%dms)\n",
                    (LORA_RX_DUTY_CYCLE_ENABLED && LORA_LIGHT_SLEEP_ENABLED) ? "ACTIVE" : "OFF",
                    LORA_RX_DUTY_CYCLE_ENABLED ? "ON" : "OFF",
                    LORA_RXPS_LEVEL,
                    LORA_LIGHT_SLEEP_ENABLED ? "ON" : "OFF",
                    LORA_LIGHT_SLEEP_PERIOD_MS);
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