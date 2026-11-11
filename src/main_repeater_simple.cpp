// ============================================================
//  LORA REPEATER SIMPLE — Trạm lặp chuyển tiếp tiết kiệm năng lượng
// ============================================================
#include "config.h"
#include <Arduino.h>
#include "lora_mesh/lora_mesh.h"

volatile unsigned long g_alarm_end_ms = 0;

void setup() {
  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LED_ACTIVE_LEVEL); // Báo nguồn hệ thống sẵn sàng

  pinMode(LED_LORA, OUTPUT);
  digitalWrite(LED_LORA, LED_INACTIVE_LEVEL); // LED LoRa tắt ở chế độ chờ

  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

#if defined(BTN_PIN) && (BTN_PIN >= 0)
  pinMode(BTN_PIN, INPUT_PULLUP);
#endif

  Serial.begin(115200);

  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 2000)) {
    delay(10);
  }
  delay(200);

  bool loraOk = lora_mesh_init();

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
  Serial.println("  Commands: [t/BTN] SOS Gateway  [p] Ping Broadcast  [h] Heartbeat  [r] Routes  [w] Watchdog  [s] Power");
  Serial.println("            ALARM:ALL:ON | ALARM:ALL:OFF | ALARM:<id>:ON | ALARM:<id>:OFF | CHAT:...");
  Serial.println("==================================================================");
  if (loraOk) {
    Serial.println("[SYS] Repeater active — listening for mesh packets...\n");
  } else {
    Serial.println("[ERR] LoRa hardware not responding — check SPI wiring!\n");
  }
}

void loop() {
  // Còi báo động (tự ngắt khi hết thời gian)
  if (g_alarm_end_ms > 0) {
    if (millis() < g_alarm_end_ms) {
      digitalWrite(ALARM_PIN, HIGH);
    } else {
      digitalWrite(ALARM_PIN, LOW);
      g_alarm_end_ms = 0;
    }
  }

  lora_mesh_loop();

  // Xử lý tập lệnh điều khiển từ cổng Serial
  static String serialBuffer = "";
  static unsigned long lastSerialActiveMs = 0;

  while (Serial.available()) {
    lastSerialActiveMs = millis(); // Giữ MCU thức 15s khi người dùng thao tác
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        if (serialBuffer.startsWith("ALARM:")) {
          lora_mesh_processAlarmCommand(serialBuffer.c_str());
          // Kích hoạt còi tại chỗ nếu nhận lệnh bật/tắt
          if (serialBuffer.indexOf("ON") != -1) {
            pinMode(ALARM_PIN, OUTPUT);
            digitalWrite(ALARM_PIN, HIGH);
            g_alarm_end_ms = millis() + ALARM_DURATION_MS;
            Serial.println("[LOCAL] ✓ Alarm hardware triggered locally (5s)");
          } else if (serialBuffer.indexOf("OFF") != -1) {
            digitalWrite(ALARM_PIN, LOW);
            g_alarm_end_ms = 0;
            Serial.println("[LOCAL] ✓ Alarm hardware stopped locally");
          }
        } else if (serialBuffer.startsWith("CHAT:")) {
          lora_mesh_processChatCommand(serialBuffer.c_str());
        } else if (serialBuffer.length() == 1) {
          char cmd = serialBuffer[0];
          if (cmd == 't' || cmd == 'T') {
            Serial.println("[CMD] Test SOS -> Gateway (dest: 0)...");
            g_alarm_end_ms = millis() + ALARM_DURATION_MS;
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
      }
      serialBuffer = "";
    } else {
      if (serialBuffer.length() < 128) {
        serialBuffer += c;
      }
    }
  }

  // Tự động gửi Heartbeat định kỳ về Gateway
  static unsigned long lastHeartbeatTx = 0;
  if (millis() - lastHeartbeatTx >= LORA_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatTx = millis();
    lora_mesh_sendHeartbeat(false);
  }

  // Light Sleep khi rảnh rỗi (không ngủ nếu đang còi, vừa thao tác Serial hoặc LED đang nhấp nháy)
#if LORA_LIGHT_SLEEP_ENABLED
  bool isSerialActive = (millis() - lastSerialActiveMs < 15000) || Serial.available();
  bool isLedBlinking = lora_mesh_isLedBlinking();
  if (g_alarm_end_ms == 0 && !isSerialActive && !isLedBlinking) {
    lora_mesh_light_sleep(LORA_LIGHT_SLEEP_PERIOD_MS);
  } else {
    delay(10);
  }
#else
  delay(10);
#endif
}
