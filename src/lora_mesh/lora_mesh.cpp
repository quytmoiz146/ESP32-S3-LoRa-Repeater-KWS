#include "lora_mesh.h"
#include "config.h"
#include <RadioLib.h>
#include <SPI.h>
#include <stddef.h>
#include <esp_sleep.h>
#include <esp_mac.h>

// ============================================================
//  Node Identity & eFuse MAC Address
// ============================================================
static uint8_t g_device_mac[6] = {0};
static char g_device_mac_str[18] = {0};
static uint8_t g_my_node_id = 0;

void lora_mesh_initNodeId() {
  if (g_my_node_id != 0) return; // Đã khởi tạo
  
  esp_err_t err = esp_efuse_mac_get_default(g_device_mac);
  if (err != ESP_OK) {
    esp_read_mac(g_device_mac, ESP_MAC_WIFI_STA);
  }

  snprintf(g_device_mac_str, sizeof(g_device_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           g_device_mac[0], g_device_mac[1], g_device_mac[2],
           g_device_mac[3], g_device_mac[4], g_device_mac[5]);

  // Dùng byte cuối cùng của MAC eFuse làm Node ID
  g_my_node_id = g_device_mac[5];

  // Tránh đụng GATEWAY_ID (0) và BROADCAST_ID (255)
  if (g_my_node_id == GATEWAY_ID) {
    g_my_node_id = 1;
  } else if (g_my_node_id == BROADCAST_ID) {
    g_my_node_id = 254;
  }
}

uint8_t lora_mesh_getNodeId() {
  if (g_my_node_id == 0) {
    lora_mesh_initNodeId();
  }
  return g_my_node_id;
}

const char* lora_mesh_getMacString() {
  if (g_device_mac_str[0] == '\0') {
    lora_mesh_initNodeId();
  }
  return g_device_mac_str;
}

// ============================================================
//  SX1262 Hardware Setup (Ra-01SH module)
// ============================================================
static SPIClass *LoRaSPI_ptr = nullptr;
static SX1262 *radio_ptr = nullptr;

#define LoRaSPI (*LoRaSPI_ptr)
#define radio (*radio_ptr)

static SemaphoreHandle_t loraMutex = NULL;
extern volatile unsigned long g_alarm_end_ms;

// ============================================================
//  ISR — Cờ báo có gói tin đến
// ============================================================
static volatile bool rx_packet_ready = false;

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
static void onDio1Interrupt() { rx_packet_ready = true; }

// ============================================================
//  Deduplication Cache
// ============================================================
#define SEEN_CACHE_SIZE 64
struct SeenPacket {
  uint8_t src_id;
  uint16_t msg_id;
  uint32_t timestamp;
};
static SeenPacket seen_cache[SEEN_CACHE_SIZE];
static uint8_t seen_idx = 0;

static bool isPacketSeen(uint8_t src_id, uint16_t msg_id) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seen_cache[i].src_id == src_id && seen_cache[i].msg_id == msg_id) {
      if (now - seen_cache[i].timestamp < 60000) { // 60 giây cho mạng lớn
        return true;
      }
    }
  }
  return false;
}

static void markPacketSeen(uint8_t src_id, uint16_t msg_id) {
  seen_cache[seen_idx].src_id = src_id;
  seen_cache[seen_idx].msg_id = msg_id;
  seen_cache[seen_idx].timestamp = millis();
  seen_idx = (seen_idx + 1) % SEEN_CACHE_SIZE;
}

// ============================================================
//  MSG ID counter
// ============================================================
static uint16_t nextMsgId = 1;

// ============================================================
//  Thống kê
// ============================================================
static uint32_t stats_rx_total = 0;
static uint32_t stats_relayed = 0;
static uint32_t stats_dropped_seen = 0;
static uint32_t stats_tx_sos = 0;
static uint32_t stats_rx_wd_resets = 0;
static uint32_t last_rx_activity_ms = 0;

uint32_t lora_mesh_getWatchdogResets() {
  return stats_rx_wd_resets;
}

// ============================================================
//  Dynamic RXPS Timings (MeshCore Derivation Formula)
// ============================================================
static uint32_t g_rxps_rx_us = LORA_RX_DUTY_CYCLE_RX_US;
static uint32_t g_rxps_sleep_us = LORA_RX_DUTY_CYCLE_SLEEP_US;

void lora_mesh_calcRxpsTimings(uint8_t level, uint8_t sf, float bw, uint8_t preamble, uint32_t* rx_us, uint32_t* sleep_us) {
  // 1. T_sym (us) = (2^SF / BW_kHz) * 1000
  float symbol_us = ((float)(1UL << sf) / bw) * 1000.0f;

  // 2. Số symbol preamble cần bắt (catch dial)
  float catch_symbols = 10.2f; // Balanced profile
  if (level == RXPS_LEVEL_MAX) {
    catch_symbols = 8.2f;
  } else if (level == RXPS_LEVEL_CONSERVATIVE) {
    catch_symbols = 13.2f;
  }

  // 3. Thời gian ngủ: sleep = (preamble - catch) * symbol_us
  float sleep_symbols = (float)preamble - catch_symbols;
  if (sleep_symbols < 0.0f) sleep_symbols = 0.0f;
  *sleep_us = (uint32_t)(sleep_symbols * symbol_us);

  // 4. Timer guard: 2 * rx + sleep >= (preamble + sync + header + slack) * symbol_us
  float sync_symbols = (sf <= 6) ? 4.0f : 2.0f;
  float need_us = ((float)preamble + sync_symbols + 8.0f + 4.0f) * symbol_us;
  float min_rx_us = (need_us - (float)(*sleep_us)) / 2.0f;
  if (min_rx_us < symbol_us) min_rx_us = symbol_us;
  *rx_us = (uint32_t)min_rx_us;

  if (*rx_us < 1000UL) *rx_us = 1000UL;
  if (*sleep_us < 1000UL) *sleep_us = 1000UL;
}

// ============================================================
//  Giám sát Pin & Bảo vệ Sụt nguồn (Brownout Protection)
// ============================================================
uint16_t lora_mesh_getBatteryMilliVolts() {
#if defined(PIN_VBAT_READ) && (PIN_VBAT_READ >= 0)
  analogReadResolution(12);
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) {
    sum += analogReadMilliVolts(PIN_VBAT_READ);
  }
  uint16_t mv = (uint16_t)((sum / 4.0f) * BATTERY_DIVIDER_RATIO);
  if (mv > 2000 && mv < 5000) return mv;
#endif
  return 3700; // Mặc định 3.7V nếu chưa gắn chân đọc ADC
}

void lora_mesh_checkBatteryProtection() {
  uint16_t mv = lora_mesh_getBatteryMilliVolts();
  if (mv >= 2000 && mv <= BATTERY_CRITICAL_MV) {
    Serial.printf("[WARN] Critical Battery (%dmV <= %dmV)! Entering Deep Sleep\n",
                  mv, BATTERY_CRITICAL_MV);
    radio.sleep();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL); // Thức sau 1h kiểm tra sạc solar
    esp_deep_sleep_start();
  }
}

// ============================================================
//  Mesh Routing Table — Reverse Path Learning & Next Hop
// ============================================================
#define MAX_ROUTES 16
struct RouteEntry {
  uint8_t  dest_id;
  uint8_t  next_hop;
  uint8_t  hop_count;
  float    last_rssi;
  uint32_t timestamp;
};
static RouteEntry routing_table[MAX_ROUTES];
static uint8_t route_count = 0;

static void updateRoute(uint8_t dest_id, uint8_t next_hop, uint8_t hop_count, float rssi) {
  if (dest_id == MY_NODE_ID || dest_id == BROADCAST_ID) return;
  for (uint8_t i = 0; i < route_count; i++) {
    if (routing_table[i].dest_id == dest_id) {
      if (hop_count <= routing_table[i].hop_count || (millis() - routing_table[i].timestamp > 60000)) {
        routing_table[i].next_hop = next_hop;
        routing_table[i].hop_count = hop_count;
        routing_table[i].last_rssi = rssi;
        routing_table[i].timestamp = millis();
      }
      return;
    }
  }
  if (route_count < MAX_ROUTES) {
    routing_table[route_count].dest_id = dest_id;
    routing_table[route_count].next_hop = next_hop;
    routing_table[route_count].hop_count = hop_count;
    routing_table[route_count].last_rssi = rssi;
    routing_table[route_count].timestamp = millis();
    route_count++;
  }
}

static uint8_t findNextHop(uint8_t dest_id) {
  for (uint8_t i = 0; i < route_count; i++) {
    if (routing_table[i].dest_id == dest_id) {
      if (millis() - routing_table[i].timestamp < 300000) { // Route hợp lệ 5 phút
        return routing_table[i].next_hop;
      }
    }
  }
  return BROADCAST_ID;
}

void lora_mesh_printRoutingTable() {
  Serial.printf("\n--- Routing Table (%u routes) ---\n", route_count);
  if (route_count == 0) {
    Serial.println("  (Empty - awaiting mesh packets)");
  } else {
    for (uint8_t i = 0; i < route_count; i++) {
      uint32_t age_s = (millis() - routing_table[i].timestamp) / 1000;
      Serial.printf("  [%u] Dest:%u -> NextHop:%u | Hops:%u | RSSI:%.1fdBm | Age:%lus%s\n",
                    i, routing_table[i].dest_id, routing_table[i].next_hop,
                    routing_table[i].hop_count, routing_table[i].last_rssi,
                    (unsigned long)age_s, (age_s > 300) ? " (EXPIRED)" : "");
    }
  }
  Serial.println("--------------------------------\n");
}

// ============================================================
//  QoS — Stop-and-Wait ARQ (ACK & Retries)
// ============================================================
struct PendingAck {
  bool        active;
  MeshPacket  pkt;
  uint32_t    sent_at;
  uint8_t     retries_left;
  uint32_t    timeout_ms;
};
static PendingAck g_pending_ack = {false};

// ============================================================
//  Dịch vụ Chat Hai Chiều (Two-Way Chat Service)
// ============================================================
struct PendingChat {
  bool     active = false;
  uint32_t client_id = 0;
  uint16_t msg_id = 0;
  uint8_t  target = 0;
  uint8_t  attempts = 0;
  uint32_t last_tx_ms = 0;
  uint32_t timeout_ms = 2000;
  uint8_t  max_retries = 3;
  char     text[128] = {};
};
static PendingChat g_pending_chat;

static ChatRecvCallback g_chat_recv_cb = nullptr;
static ChatStatusCallback g_chat_status_cb = nullptr;

#define CHAT_SEEN_CACHE_SIZE 16
struct ChatSeenEntry {
  uint8_t  src_id;
  uint16_t msg_id;
  uint32_t timestamp;
};
static ChatSeenEntry g_chat_seen_cache[CHAT_SEEN_CACHE_SIZE];
static uint8_t g_chat_seen_idx = 0;

static bool isChatSeen(uint8_t src_id, uint16_t msg_id) {
  for (uint8_t i = 0; i < CHAT_SEEN_CACHE_SIZE; i++) {
    if (g_chat_seen_cache[i].src_id == src_id && g_chat_seen_cache[i].msg_id == msg_id) {
      if (millis() - g_chat_seen_cache[i].timestamp < 60000) {
        return true;
      }
    }
  }
  return false;
}

static void markChatSeen(uint8_t src_id, uint16_t msg_id) {
  g_chat_seen_cache[g_chat_seen_idx].src_id = src_id;
  g_chat_seen_cache[g_chat_seen_idx].msg_id = msg_id;
  g_chat_seen_cache[g_chat_seen_idx].timestamp = millis();
  g_chat_seen_idx = (g_chat_seen_idx + 1) % CHAT_SEEN_CACHE_SIZE;
}

void lora_mesh_setChatCallback(ChatRecvCallback cb) {
  g_chat_recv_cb = cb;
}

void lora_mesh_setChatStatusCallback(ChatStatusCallback cb) {
  g_chat_status_cb = cb;
}

bool lora_mesh_isChatPending() {
  return g_pending_chat.active;
}

void lora_mesh_emitChatStatus(uint32_t client_id, uint16_t msg_id, const char* status, const char* reason) {
  if (g_chat_status_cb) {
    g_chat_status_cb(client_id, msg_id, status, reason);
  }
  if (reason) {
    Serial.printf("{\"type\":\"CHAT_STATUS\",\"client_id\":%lu,\"msg_id\":%u,\"status\":\"%s\",\"reason\":\"%s\"}\n",
                  (unsigned long)client_id, msg_id, status, reason);
  } else {
    Serial.printf("{\"type\":\"CHAT_STATUS\",\"client_id\":%lu,\"msg_id\":%u,\"status\":\"%s\"}\n",
                  (unsigned long)client_id, msg_id, status);
  }
}

// ============================================================
//  Helpers
// ============================================================
static String formatPath(const MeshPacket &pkt) {
  String s = "[" + String(pkt.src_id);
  for (int i = 0; i < pkt.hop_count && i < MAX_PATH_LEN; i++) {
    s += " -> " + String(pkt.path[i]);
  }
  s += "]";
  return s;
}

static bool waitBusy(uint32_t timeoutMs = 100) {
  unsigned long start = millis();
  while (digitalRead(LORA_BUSY_PIN) == HIGH) {
    if (millis() - start > timeoutMs) {
      return false; // Timeout — radio bị kẹt
    }
    delay(1);
  }
  return true;
}

static void hardResetRadio() {
  Serial.println("[WARN] SX1262 hardware reset...");
  digitalWrite(LORA_RST_PIN, LOW);
  delay(20);
  digitalWrite(LORA_RST_PIN, HIGH);
  delay(50);
  waitBusy(200);
}

static bool reinitRadio() {
  hardResetRadio();
  int16_t state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                              RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                              LORA_TCXO_VOLTAGE, false);
  if (state != RADIOLIB_ERR_NONE) {
    float alt_tcxo = (LORA_TCXO_VOLTAGE > 0.0) ? 0.0 : 1.8;
    state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                        alt_tcxo, false);
  }
  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(onDio1Interrupt);
  return (state == RADIOLIB_ERR_NONE);
}

static void enterReceiveMode() {
  int16_t state = RADIOLIB_ERR_NONE;

#if LORA_RX_DUTY_CYCLE_ENABLED
  // Bật chế độ thu chu kỳ tiết kiệm điện động (Dynamic RXPS)
  state = radio.startReceiveDutyCycle(g_rxps_rx_us, g_rxps_sleep_us);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] RX duty cycle fail: %d -> fallback continuous RX\n", state);
    state = radio.startReceive();
  }
#else
  state = radio.startReceive();
#endif

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] startReceive fail: %d -> attempting recovery\n", state);
    reinitRadio();

#if LORA_RX_DUTY_CYCLE_ENABLED
    state = radio.startReceiveDutyCycle(g_rxps_rx_us, g_rxps_sleep_us);
    if (state != RADIOLIB_ERR_NONE) {
      state = radio.startReceive();
    }
#else
    state = radio.startReceive();
#endif

    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("[ERR] Recovery fail: %d\n", state);
    } else {
      Serial.println("[SYS] Recovery OK -> RX mode restored");
    }
  }
}

// ============================================================
//  RX Watchdog (RXPS Watchdog — MeshCore style)
// ============================================================
static void checkRxWatchdog() {
#if LORA_RX_WATCHDOG_ENABLED
  // Kiểm tra sức khỏe radio định kỳ mỗi 5 giây
  static unsigned long lastWdCheck = 0;
  if (millis() - lastWdCheck < 5000) return;
  lastWdCheck = millis();

  if (last_rx_activity_ms == 0) {
    last_rx_activity_ms = millis();
    return;
  }

  // 1. Kiểm tra kẹt phần cứng: Chân BUSY ở mức HIGH liên tục
  if (digitalRead(LORA_BUSY_PIN) == HIGH) {
    unsigned long waitBusyStart = millis();
    while (digitalRead(LORA_BUSY_PIN) == HIGH && (millis() - waitBusyStart < 50)) {
      delay(2);
    }
    if (digitalRead(LORA_BUSY_PIN) == HIGH) {
      stats_rx_wd_resets++;
      Serial.printf("[WD] BUSY stuck HIGH! Hard reset SX1262 (Resets: %lu)\n",
                    (unsigned long)stats_rx_wd_resets);
      reinitRadio();
      enterReceiveMode();
      last_rx_activity_ms = millis();
      return;
    }
  }

  // 2. Kiểm tra giao tiếp SPI & thanh ghi IRQ
  uint16_t irq = radio.getIrqStatus();
  if (irq == 0xFFFF) {
    stats_rx_wd_resets++;
    Serial.printf("[WD] Radio unresponsive (IRQ=0xFFFF)! Hard reset (Resets: %lu)\n",
                  (unsigned long)stats_rx_wd_resets);
    reinitRadio();
    enterReceiveMode();
    last_rx_activity_ms = millis();
    return;
  }

  // 3. Chu kỳ im lặng quá lâu (Silent watchdog / RXPS timer refresh)
  if (millis() - last_rx_activity_ms >= LORA_RX_WATCHDOG_TIMEOUT_MS) {
    last_rx_activity_ms = millis();
    enterReceiveMode();
#if LORA_RX_DUTY_CYCLE_ENABLED
    Serial.println("[WD] Periodic RXPS refresh — duty cycle resynced");
#endif
  }
#endif
}

// ============================================================
//  CAD — Kiểm tra kênh trước khi phát (tránh va chạm sóng)
// ============================================================
bool lora_mesh_isChannelActive() {
  if (!radio_ptr) return false;

  int16_t cadResult = radio.scanChannel();

  // scanChannel() kích hoạt ngắt CAD_DONE trên DIO1, xóa cờ để tránh hiểu nhầm có gói RX
  radio.clearIrqStatus();
  rx_packet_ready = false;

  return (cadResult == RADIOLIB_LORA_DETECTED);
}

static void waitChannelFreeWithCAD() {
#if LORA_CAD_ENABLED
  uint8_t retries = 0;
  while (retries < LORA_CAD_MAX_RETRIES && lora_mesh_isChannelActive()) {
    retries++;
    uint32_t backoff = random(40, 120);
    Serial.printf("[CAD] Channel busy, backoff %lums (%d/%d)\n",
                  (unsigned long)backoff, retries, LORA_CAD_MAX_RETRIES);
    delay(backoff);
  }
  if (retries >= LORA_CAD_MAX_RETRIES) {
    Serial.println("[CAD] Channel busy after max retries -> forcing transmit");
  }
#endif
}

// ============================================================
//  Helper: Thực thi lệnh truyền (dùng chung cho send và relay)
// ============================================================
static int executeTransmit(MeshPacket &pkt) {
  // 1. Kiểm tra kênh CAD trước khi phát
  waitChannelFreeWithCAD();

  // 2. Chỉ phát đúng kích thước Header + chiều dài Payload thực tế (Dynamic Length)
  size_t txLen = MESH_HEADER_SIZE + pkt.payload_len;
  if (txLen > sizeof(MeshPacket)) {
    txLen = sizeof(MeshPacket);
  }

  int16_t state = radio.transmit((uint8_t*)&pkt, txLen);
  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] LoRa TX fail: %d\n", state);
  }
  
  // Luôn luôn quay về chế độ RX — kể cả khi TX lỗi
  enterReceiveMode();
  last_rx_activity_ms = millis();
  return state;
}

// ============================================================
//  Relay — Phát chuyển tiếp (có CSMA backoff)
// ============================================================
static bool relayPacket(MeshPacket &pkt) {
  // Time-slot backoff dựa trên NODE_ID
  uint32_t base_slot = (MY_NODE_ID % 2 == 0) ? 60 : 260;
  uint32_t jitter = base_slot + random(10, 90);
  delay(jitter);

  int state = executeTransmit(pkt);

  if (state == RADIOLIB_ERR_NONE) {
    return true;
  } else {
    Serial.printf("[ERR] Relay TX fail: %d\n", state);
    return false;
  }
}

// ============================================================
//  QoS — Gửi gói tin xác nhận ACK
// ============================================================
static void sendAckPacket(uint8_t dest_id, uint16_t ack_msg_id) {
  MeshPacket ackPkt;
  memset(&ackPkt, 0, sizeof(ackPkt));
  ackPkt.magic = MESH_MAGIC;
  ackPkt.msg_id = ack_msg_id;
  ackPkt.msg_type = PKT_TYPE_ACK;
  ackPkt.flags = PKT_FLAG_IS_ACK;
  ackPkt.src_id = MY_NODE_ID;
  ackPkt.dest_id = dest_id;
#if LORA_UNICAST_ROUTING_ENABLED
  ackPkt.next_hop = findNextHop(dest_id);
#else
  ackPkt.next_hop = BROADCAST_ID;
#endif
  ackPkt.sender_id = MY_NODE_ID;
  ackPkt.hop_count = 0;
  ackPkt.payload_len = 0;

  if (ackPkt.next_hop != BROADCAST_ID) {
    Serial.printf("[TX] ACK #%u -> Node %u (NextHop:%u)\n", ack_msg_id, dest_id, ackPkt.next_hop);
  } else {
    Serial.printf("[TX] ACK #%u -> Node %u (Flood)\n", ack_msg_id, dest_id);
  }
  executeTransmit(ackPkt);
}

// ============================================================
//  Thực thi gửi & thử lại tin nhắn Chat (Stop-and-Wait ARQ)
// ============================================================
static void transmitPendingChat() {
  if (!g_pending_chat.active) return;
  MeshPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = MESH_MAGIC;
  pkt.msg_id = g_pending_chat.msg_id;
  pkt.msg_type = PKT_TYPE_CHAT;
  pkt.flags = PKT_FLAG_ACK_REQ;
  pkt.src_id = MY_NODE_ID;
  pkt.dest_id = g_pending_chat.target;
#if LORA_UNICAST_ROUTING_ENABLED
  pkt.next_hop = findNextHop(g_pending_chat.target);
#else
  pkt.next_hop = BROADCAST_ID;
#endif
  pkt.sender_id = MY_NODE_ID;
  pkt.hop_count = 0;
  pkt.path[0] = MY_NODE_ID;

  size_t len = strlen(g_pending_chat.text);
  if (len > sizeof(pkt.payload) - 1) len = sizeof(pkt.payload) - 1;
  memcpy(pkt.payload, g_pending_chat.text, len);
  pkt.payload_len = len;

  g_pending_chat.attempts++;
  g_pending_chat.last_tx_ms = millis();

  if (g_pending_chat.attempts == 1) {
    lora_mesh_emitChatStatus(g_pending_chat.client_id, g_pending_chat.msg_id, "SENDING");
  }

  Serial.printf("[CHAT] TX #%u -> Node %u (Attempt %d/%d, NextHop:%u): \"%s\"\n",
                g_pending_chat.msg_id, g_pending_chat.target,
                g_pending_chat.attempts, g_pending_chat.max_retries,
                pkt.next_hop, g_pending_chat.text);

  int state = executeTransmit(pkt);
  if (state != RADIOLIB_ERR_NONE && g_pending_chat.attempts >= g_pending_chat.max_retries) {
    lora_mesh_emitChatStatus(g_pending_chat.client_id, g_pending_chat.msg_id, "FAILED", "RADIO");
    g_pending_chat.active = false;
  }
}

static void tickPendingChat() {
  if (!g_pending_chat.active) return;
  if (millis() - g_pending_chat.last_tx_ms < g_pending_chat.timeout_ms) return;

  if (g_pending_chat.attempts >= g_pending_chat.max_retries) {
    Serial.printf("[CHAT] FAIL: No ACK for msg #%u after %d attempts!\n",
                  g_pending_chat.msg_id, g_pending_chat.max_retries);
    lora_mesh_emitChatStatus(g_pending_chat.client_id, g_pending_chat.msg_id, "FAILED", "NO_ACK");
    g_pending_chat.active = false;
  } else {
    transmitPendingChat();
  }
}

bool lora_mesh_sendChat(uint8_t target, const char* text, uint32_t client_id) {
  if (!text || !text[0]) {
    lora_mesh_emitChatStatus(client_id, 0, "FAILED", "BAD_FORMAT");
    return false;
  }
  if (target == MY_NODE_ID) {
    lora_mesh_emitChatStatus(client_id, 0, "FAILED", "BAD_TARGET");
    return false;
  }
  if (strlen(text) >= sizeof(g_pending_chat.text)) {
    lora_mesh_emitChatStatus(client_id, 0, "FAILED", "TOO_LONG");
    return false;
  }
  if (g_pending_chat.active) {
    lora_mesh_emitChatStatus(client_id, 0, "FAILED", "BUSY");
    return false;
  }

  g_pending_chat.active = true;
  g_pending_chat.client_id = client_id;
  g_pending_chat.msg_id = nextMsgId++;
  g_pending_chat.target = target;
  g_pending_chat.attempts = 0;
  g_pending_chat.timeout_ms = 2000;
  g_pending_chat.max_retries = 3;
  strncpy(g_pending_chat.text, text, sizeof(g_pending_chat.text) - 1);
  g_pending_chat.text[sizeof(g_pending_chat.text) - 1] = '\0';

  transmitPendingChat();
  return true;
}

bool lora_mesh_processChatCommand(const char* line) {
  if (!line || strncmp(line, "CHAT:", 5) != 0) return false;
  const char* p1 = strchr(line + 5, ':');
  if (!p1) {
    lora_mesh_emitChatStatus(0, 0, "FAILED", "BAD_FORMAT");
    return false;
  }
  const char* p2 = strchr(p1 + 1, ':');
  if (!p2) {
    lora_mesh_emitChatStatus(0, 0, "FAILED", "BAD_FORMAT");
    return false;
  }

  uint32_t clientId = strtoul(line + 5, nullptr, 10);
  uint8_t target = (uint8_t)strtoul(p1 + 1, nullptr, 10);
  const char* message = p2 + 1;

  char cleanMsg[128] = {};
  strncpy(cleanMsg, message, sizeof(cleanMsg) - 1);
  size_t l = strlen(cleanMsg);
  while (l > 0 && (cleanMsg[l - 1] == '\r' || cleanMsg[l - 1] == '\n')) {
    cleanMsg[--l] = '\0';
  }

  return lora_mesh_sendChat(target, cleanMsg, clientId);
}

// ============================================================
//  Xử lý gói tin đến
// ============================================================
static void processIncomingPacket() {
  uint16_t irq = radio.getIrqStatus();
  if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
    // Không có gói tin thật — không chạm vào radio, chỉ bỏ qua
    return;
  }

  MeshPacket pkt;
  memset(&pkt, 0, sizeof(pkt));

  size_t rxLen = radio.getPacketLength();
  int state = radio.readData((uint8_t *)&pkt, sizeof(pkt));
  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  last_rx_activity_ms = millis();
  enterReceiveMode();

  if (state != RADIOLIB_ERR_NONE) {
    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[DROP] CRC mismatch (noise / corrupted)");
    } else {
      Serial.printf("[ERR] Radio read fail: %d\n", state);
    }
    return;
  }

  // 1. Kiểm tra kích thước gói tối thiểu và Magic byte
  if (rxLen > 0 && rxLen < MESH_HEADER_SIZE) {
    Serial.printf("[DROP] Short packet (%d < %d bytes)\n", (int)rxLen, (int)MESH_HEADER_SIZE);
    return;
  }

  if (pkt.magic != MESH_MAGIC) {
    Serial.printf("[DROP] Invalid magic 0x%02X (exp: 0x%02X)\n", pkt.magic, MESH_MAGIC);
    return;
  }

  if (pkt.payload_len > sizeof(pkt.payload) - 1) {
    pkt.payload_len = sizeof(pkt.payload) - 1;
  }
  if (!(pkt.flags & PKT_FLAG_BINARY)) {
    pkt.payload[pkt.payload_len] = '\0';
  }

  stats_rx_total++;

  // 2. Không relay gói do chính mình gửi
  if (pkt.src_id == MY_NODE_ID) {
    return;
  }

  // 3. Deduplication (chống lặp gói)
  if (isPacketSeen(pkt.src_id, pkt.msg_id)) {
    stats_dropped_seen++;
    // Nếu là gói tin Chat gửi đích danh cho mình mà bị lặp (do gói ACK trước bị rơi trên đường truyền):
    // Bắt buộc phải phát lại gói ACK để giải phóng hàng đợi retry của bên gửi!
    if (pkt.msg_type == PKT_TYPE_CHAT && pkt.dest_id == MY_NODE_ID) {
      Serial.printf("[CHAT] Duplicate msg #%u from Node %u -> Resending ACK\n", pkt.msg_id, pkt.src_id);
      sendAckPacket(pkt.src_id, pkt.msg_id);
    }
    return;
  }
  markPacketSeen(pkt.src_id, pkt.msg_id);

  // 4. Mesh Routing: Học đường truyền ngược (Reverse Path Learning)
  updateRoute(pkt.src_id, pkt.sender_id, pkt.hop_count, rssi);

  const char *typeStr = "DATA";
  if (pkt.msg_type == PKT_TYPE_SOS) {
    typeStr = "SOS";
    g_alarm_end_ms = millis() + 7000; // Bật còi 7s khi nhận được SOS (cả repeater cũng kêu)
  }
  else if (pkt.msg_type == PKT_TYPE_ACK || pkt.msg_type == PKT_TYPE_CHAT_ACK) {
    typeStr = "ACK";
  }
  else if (pkt.msg_type == PKT_TYPE_PING) {
    typeStr = "PING";
  }
  else if (pkt.msg_type == PKT_TYPE_CHAT) {
    typeStr = "CHAT";
  }

  char payloadInfo[64] = {0};
  if (pkt.flags & PKT_FLAG_BINARY) {
    if (pkt.payload_len >= sizeof(SosBinaryPayload)) {
      SosBinaryPayload* bin = (SosBinaryPayload*)pkt.payload;
      if (bin->alert_type == ALERT_HEARTBEAT) {
        typeStr = "HEARTBEAT";
        snprintf(payloadInfo, sizeof(payloadInfo), "Batt:%.2fV", bin->battery_mv / 1000.0f);
        if (pkt.dest_id == MY_NODE_ID || pkt.dest_id == GATEWAY_ID) {
          Serial.printf("{\"type\":\"HEARTBEAT\",\"src\":%u,\"battery\":%u,\"role\":\"REPEATER\",\"rssi\":%.1f,\"snr\":%.1f}\n",
                        pkt.src_id, bin->battery_mv, rssi, snr);
        }
      } else {
        const char* alertName = (bin->alert_type == ALERT_BAWL) ? "BAWL" : 
                                (bin->alert_type == ALERT_TEST) ? "TEST" : "ALERT";
        const char* dirName = (bin->direction == DIR_LEFT) ? "L" :
                              (bin->direction == DIR_RIGHT) ? "R" : "C";
        snprintf(payloadInfo, sizeof(payloadInfo), "%s %d%% [%s] | Batt:%.2fV",
                 alertName, bin->confidence, dirName, bin->battery_mv / 1000.0f);
        if (pkt.dest_id == MY_NODE_ID || pkt.dest_id == GATEWAY_ID) {
          Serial.printf("{\"type\":\"SOS\",\"src\":%u,\"alert\":\"%s\",\"confidence\":%u,\"dir\":\"%s\",\"battery\":%u,\"msg_id\":%u,\"rssi\":%.1f,\"snr\":%.1f}\n",
                        pkt.src_id, alertName, bin->confidence, dirName, bin->battery_mv, pkt.msg_id, rssi, snr);
        }
      }
    }
  } else if (pkt.msg_type == PKT_TYPE_ACK || pkt.msg_type == PKT_TYPE_CHAT_ACK) {
    snprintf(payloadInfo, sizeof(payloadInfo), "ACK-OK");
  } else if (pkt.payload_len > 0) {
    snprintf(payloadInfo, sizeof(payloadInfo), "\"%.32s\"", pkt.payload);
  }

  if (pkt.next_hop != BROADCAST_ID) {
    Serial.printf(
        "[RX] %s #%u | Src:%u->Dest:%u (NextHop:%u, via:%u, hop:%u) | %.0fdBm/%.1fdB | %s | Path:%s\n",
        typeStr, pkt.msg_id, pkt.src_id, pkt.dest_id, pkt.next_hop, pkt.sender_id, pkt.hop_count,
        rssi, snr, payloadInfo[0] ? payloadInfo : "none",
        formatPath(pkt).c_str());
  } else {
    Serial.printf(
        "[RX] %s #%u | Src:%u->Dest:%u (Flood, via:%u, hop:%u) | %.0fdBm/%.1fdB | %s | Path:%s\n",
        typeStr, pkt.msg_id, pkt.src_id, pkt.dest_id, pkt.sender_id, pkt.hop_count,
        rssi, snr, payloadInfo[0] ? payloadInfo : "none",
        formatPath(pkt).c_str());
  }

  // 5. QoS: Xử lý khi nhận được gói ACK phản hồi
  if ((pkt.flags & PKT_FLAG_IS_ACK) || pkt.msg_type == PKT_TYPE_ACK || pkt.msg_type == PKT_TYPE_CHAT_ACK) {
    // 5.1. Xác nhận ACK cho gói SOS / Data thông thường
    if (g_pending_ack.active && g_pending_ack.pkt.msg_id == pkt.msg_id) {
      uint32_t rtt = millis() - g_pending_ack.sent_at;
      g_pending_ack.active = false;
      Serial.printf("[QoS] ✓ ACK CONFIRMED for msg #%u from Node %u (RTT: %lums)\n",
                    pkt.msg_id, pkt.src_id, (unsigned long)rtt);
    }
    // 5.2. Xác nhận ACK cho gói Chat (yêu cầu từ chính đích cuối khớp target và msg_id)
    if (g_pending_chat.active && g_pending_chat.msg_id == pkt.msg_id && g_pending_chat.target == pkt.src_id) {
      uint32_t rtt = millis() - g_pending_chat.last_tx_ms;
      g_pending_chat.active = false;
      lora_mesh_emitChatStatus(g_pending_chat.client_id, pkt.msg_id, "DELIVERED");
      Serial.printf("[CHAT] ✓ DELIVERED msg #%u to Node %u (RTT: %lums)\n",
                    pkt.msg_id, pkt.src_id, (unsigned long)rtt);
    }
    // Gói ACK đã tới đích cuối thì không relay tiếp
    if (pkt.dest_id == MY_NODE_ID) return;
  }

  // 6. Xử lý khi nhận tin nhắn Chat gửi đích danh cho node này
  if (pkt.msg_type == PKT_TYPE_CHAT && (pkt.dest_id == MY_NODE_ID || pkt.dest_id == BROADCAST_ID)) {
    // Xuất chuỗi NDJSON cho PC / Smartphone App
    Serial.printf("{\"type\":\"CHAT\",\"src\":%u,\"dest\":%u,\"text\":\"%s\",\"msg_id\":%u,\"hops\":%u,\"rssi\":%.1f,\"snr\":%.1f}\n",
                  pkt.src_id, pkt.dest_id, pkt.payload, pkt.msg_id, pkt.hop_count, rssi, snr);
    if (g_chat_recv_cb) {
      g_chat_recv_cb(pkt.src_id, pkt.dest_id, pkt.payload, pkt.msg_id, pkt.hop_count, rssi, snr);
    }
    // Phát ACK xác nhận cho bên gửi
    sendAckPacket(pkt.src_id, pkt.msg_id);
    if (pkt.dest_id == MY_NODE_ID) return; // Dừng relay
  }

  // 6.5. QoS: Phản hồi ACK nếu gói tin SOS/Data yêu cầu
  if (pkt.flags & PKT_FLAG_ACK_REQ) {
    if (pkt.dest_id == MY_NODE_ID || pkt.dest_id == BROADCAST_ID) {
      sendAckPacket(pkt.src_id, pkt.msg_id);
    }
  }

  // Nếu gói tin gửi đích danh cho node này, dừng relay
  if (pkt.dest_id == MY_NODE_ID) {
    return;
  }

#if LORA_UNICAST_ROUTING_ENABLED
  // 6.5. Unicast Next-Hop Filtering:
  // Nếu gói tin chỉ định next_hop cụ thể khác BROADCAST_ID, và ta không phải next_hop đó
  // thì BỎ QUA không relay để tránh bão mạng và tiết kiệm pin toàn mạng!
  if (pkt.dest_id != BROADCAST_ID && pkt.next_hop != BROADCAST_ID) {
    if (pkt.next_hop != MY_NODE_ID) {
      Serial.printf("[SKIP] %s #%u: Unicast targeted to NextHop %u (self is %u)\n",
                    typeStr, pkt.msg_id, pkt.next_hop, MY_NODE_ID);
      return;
    }
  }
#endif

  // 7. Hop Limit — Giới hạn số hop (chống flood bão mạng)
  if (pkt.hop_count >= 7) {
    Serial.printf("[DROP] %s #%u: Hop limit reached (%d hops)\n", typeStr, pkt.msg_id, pkt.hop_count);
    return;
  }

  // 8. Loop prevention — chống vòng lặp kín
  for (int i = 0; i < pkt.hop_count && i < MAX_PATH_LEN; i++) {
    if (pkt.path[i] == MY_NODE_ID) {
      Serial.printf("[DROP] %s #%u: Loop detected (self in path)\n", typeStr, pkt.msg_id);
      return;
    }
  }

  // 9. Cập nhật path + hop + next_hop
  if (pkt.hop_count < MAX_PATH_LEN) {
    pkt.path[pkt.hop_count] = MY_NODE_ID;
  }
  pkt.hop_count++;
  pkt.sender_id = MY_NODE_ID;

#if LORA_UNICAST_ROUTING_ENABLED
  pkt.next_hop = (pkt.dest_id == BROADCAST_ID) ? BROADCAST_ID : findNextHop(pkt.dest_id);
#else
  pkt.next_hop = BROADCAST_ID;
#endif

  // 10. Relay
  if (relayPacket(pkt)) {
    stats_relayed++;
    if (pkt.next_hop != BROADCAST_ID) {
      Serial.printf("[RELAY] %s #%u (Unicast -> NextHop:%u) -> Hop %u OK (Path:%s)\n",
                    typeStr, pkt.msg_id, pkt.next_hop, pkt.hop_count, formatPath(pkt).c_str());
    } else {
      Serial.printf("[RELAY] %s #%u (Flood) -> Hop %u OK (Path:%s)\n",
                    typeStr, pkt.msg_id, pkt.hop_count, formatPath(pkt).c_str());
    }
  } else {
    Serial.printf("[RELAY] %s #%u -> Hop %u FAILED\n", typeStr, pkt.msg_id, pkt.hop_count);
  }
}

// ============================================================
//  Hàm gửi gói tin nội bộ (hỗ trợ Binary, Text và cờ QoS)
// ============================================================
static bool sendPacketInternal(uint8_t msgType, uint8_t flags, const char *payload,
                               uint8_t payloadLen, uint8_t destId, bool requireAck) {
  if (loraMutex && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[ERR] TX fail: Radio busy (mutex timeout)");
    return false;
  }

  MeshPacket pkt;
  memset(&pkt, 0, sizeof(pkt));

  pkt.magic = MESH_MAGIC;
  pkt.msg_id = nextMsgId++;
  pkt.msg_type = msgType;
  pkt.flags = flags;
  pkt.src_id = MY_NODE_ID;
  pkt.dest_id = destId;
#if LORA_UNICAST_ROUTING_ENABLED
  pkt.next_hop = (destId == BROADCAST_ID) ? BROADCAST_ID : findNextHop(destId);
#else
  pkt.next_hop = BROADCAST_ID;
#endif
  pkt.sender_id = MY_NODE_ID;
  pkt.hop_count = 0;
  memset(pkt.path, 0, sizeof(pkt.path));

  pkt.payload_len = (payloadLen > (sizeof(pkt.payload) - 1)) ? (sizeof(pkt.payload) - 1) : payloadLen;
  memcpy(pkt.payload, payload, pkt.payload_len);
  if (!(flags & PKT_FLAG_BINARY)) {
    pkt.payload[pkt.payload_len] = '\0';
  }

  // Đánh dấu gói chính mình gửi vào seen cache (tránh relay lại)
  markPacketSeen(pkt.src_id, pkt.msg_id);

  const char *typeStr = (msgType == PKT_TYPE_SOS) ? "SOS" :
                        (msgType == PKT_TYPE_ACK) ? "ACK" : "DATA";
  char payloadSummary[48] = {0};
  if (flags & PKT_FLAG_BINARY) {
    if (payloadLen >= sizeof(SosBinaryPayload)) {
      SosBinaryPayload* bin = (SosBinaryPayload*)payload;
      if (bin->alert_type == ALERT_HEARTBEAT) {
        typeStr = "HEARTBEAT";
        snprintf(payloadSummary, sizeof(payloadSummary), "Batt:%.2fV", bin->battery_mv / 1000.0f);
      } else {
        const char* alertName = (bin->alert_type == ALERT_BAWL) ? "BAWL" : 
                                (bin->alert_type == ALERT_TEST) ? "TEST" : "ALERT";
        const char* dirName = (bin->direction == DIR_LEFT) ? "L" :
                              (bin->direction == DIR_RIGHT) ? "R" : "C";
        snprintf(payloadSummary, sizeof(payloadSummary), "%s %d%% [%s] | Batt:%.2fV",
                 alertName, bin->confidence, dirName, bin->battery_mv / 1000.0f);
      }
    }
  } else if (payloadLen > 0) {
    snprintf(payloadSummary, sizeof(payloadSummary), "\"%.20s\"", payload);
  }

  // Đăng ký vào hàng đợi QoS nếu yêu cầu ACK
  if (requireAck) {
    g_pending_ack.active = true;
    g_pending_ack.pkt = pkt;
    g_pending_ack.sent_at = millis();
    g_pending_ack.retries_left = LORA_QOS_MAX_RETRIES;
    g_pending_ack.timeout_ms = LORA_QOS_ACK_TIMEOUT_MS;
  }

  int state = executeTransmit(pkt);

  if (state == RADIOLIB_ERR_NONE) {
    if (msgType == PKT_TYPE_SOS) stats_tx_sos++;
    if (pkt.next_hop != BROADCAST_ID) {
      Serial.printf("[TX] %s #%u -> Dest:%u (NextHop:%u%s) | %s | OK\n",
                    typeStr, pkt.msg_id, destId, pkt.next_hop,
                    requireAck ? ", wait ACK" : "",
                    payloadSummary[0] ? payloadSummary : "len:0");
    } else {
      Serial.printf("[TX] %s #%u -> Dest:%u (Flood%s) | %s | OK\n",
                    typeStr, pkt.msg_id, destId,
                    requireAck ? ", wait ACK" : "",
                    payloadSummary[0] ? payloadSummary : "len:0");
    }
    if (loraMutex) xSemaphoreGive(loraMutex);
    return true;
  } else {
    Serial.printf("[TX] %s #%u -> Dest:%u FAILED (%d)\n", typeStr, pkt.msg_id, destId, state);
    if (requireAck) g_pending_ack.active = false;
    if (loraMutex) xSemaphoreGive(loraMutex);
    return false;
  }
}

// ============================================================
//  PUBLIC API
// ============================================================

bool lora_mesh_sendSosBinary(AlertType alert, uint8_t confidence, DirectionType dir, bool requireAck, uint8_t destId) {
  SosBinaryPayload bin;
  bin.alert_type = (uint8_t)alert;
  bin.confidence = confidence;
  bin.direction = (uint8_t)dir;
  bin.battery_mv = lora_mesh_getBatteryMilliVolts();

  uint8_t flags = PKT_FLAG_BINARY;
  if (requireAck) flags |= PKT_FLAG_ACK_REQ;

  return sendPacketInternal(PKT_TYPE_SOS, flags, (const char*)&bin, sizeof(bin), destId, requireAck);
}

bool lora_mesh_sendHeartbeat(bool requireAck) {
  SosBinaryPayload bin;
  bin.alert_type = (uint8_t)ALERT_HEARTBEAT;
  bin.confidence = 0;
  bin.direction = (uint8_t)DIR_CENTER;
  bin.battery_mv = lora_mesh_getBatteryMilliVolts();

  uint8_t flags = PKT_FLAG_BINARY;
  if (requireAck) flags |= PKT_FLAG_ACK_REQ;

  return sendPacketInternal(PKT_TYPE_DATA, flags, (const char*)&bin, sizeof(bin), GATEWAY_ID, requireAck);
}

bool lora_mesh_sendSOS(const char *payload, uint8_t payloadLen, bool requireAck) {
  uint8_t flags = PKT_FLAG_NONE;
  if (requireAck) flags |= PKT_FLAG_ACK_REQ;
  return sendPacketInternal(PKT_TYPE_SOS, flags, payload, payloadLen, GATEWAY_ID, requireAck);
}

bool lora_mesh_init() {
  if (!loraMutex) {
    loraMutex = xSemaphoreCreateMutex();
  }

  if (!LoRaSPI_ptr) {
    LoRaSPI_ptr = new SPIClass(HSPI);
  }
  if (!radio_ptr) {
    radio_ptr = new SX1262(new Module(LORA_CS_PIN, LORA_DIO1_PIN, LORA_RST_PIN,
                                      LORA_BUSY_PIN, *LoRaSPI_ptr));
  }

// Giải phóng GPIO khỏi USB PHY và set OUTPUT cho RF Switch
#if defined(LORA_TXEN_PIN) && (LORA_TXEN_PIN >= 0)
  gpio_reset_pin((gpio_num_t)LORA_TXEN_PIN);
  pinMode(LORA_TXEN_PIN, OUTPUT);
  digitalWrite(LORA_TXEN_PIN, LOW);
#endif
#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
  gpio_reset_pin((gpio_num_t)LORA_RXEN_PIN);
  pinMode(LORA_RXEN_PIN, OUTPUT);
  digitalWrite(LORA_RXEN_PIN, LOW);
#endif
#if defined(LORA_DIO1_PIN) && (LORA_DIO1_PIN >= 0)
  gpio_reset_pin((gpio_num_t)LORA_DIO1_PIN);
  pinMode(LORA_DIO1_PIN, INPUT_PULLDOWN);
#endif

  // Hardware reset
  pinMode(LORA_CS_PIN, OUTPUT);
  digitalWrite(LORA_CS_PIN, HIGH);
#if defined(LORA_RST_PIN) && (LORA_RST_PIN >= 0)
  pinMode(LORA_RST_PIN, OUTPUT);
  digitalWrite(LORA_RST_PIN, LOW);
  delay(10);
  digitalWrite(LORA_RST_PIN, HIGH);
  delay(20);
#endif

  // SPI init — LoRa chiếm SPI2 với pin riêng
  LoRaSPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN,
                -1); // -1: RadioLib quản lý CS
  pinMode(LORA_BUSY_PIN, INPUT);

  // Wait for BUSY to go LOW after reset
  unsigned long startWait = millis();
  while (digitalRead(LORA_BUSY_PIN) == HIGH && (millis() - startWait) < 500) {
    delay(1);
  }

  // Tự động tính toán chu kỳ RXPS động từ SF và Bandwidth
#if LORA_RX_DUTY_CYCLE_ENABLED
  lora_mesh_calcRxpsTimings(LORA_RXPS_LEVEL, LORA_SPREADING_FACTOR, LORA_BANDWIDTH,
                            LORA_PREAMBLE_LEN, &g_rxps_rx_us, &g_rxps_sleep_us);
#endif

  // Radio init
  int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                          LORA_TCXO_VOLTAGE, false);

  if (state != RADIOLIB_ERR_NONE) {
    float alt_tcxo = (LORA_TCXO_VOLTAGE > 0.0) ? 0.0 : 1.8;
    state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                        alt_tcxo, false);
  }

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERR] SX1262 init failed: %d\n", state);
    return false;
  }

// RF switch pins
#if defined(LORA_RXEN_PIN) && defined(LORA_TXEN_PIN) &&                        \
    (LORA_RXEN_PIN >= 0) && (LORA_TXEN_PIN >= 0)
  radio.setRfSwitchPins(LORA_RXEN_PIN, LORA_TXEN_PIN);
#endif

  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(onDio1Interrupt);
  enterReceiveMode();

  randomSeed(esp_random());
  return true;
}

void lora_mesh_loop() {
  if (!loraMutex) return;

  // Cố gắng lấy mutex
  if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  // 1. Kiểm tra IRQ polling (backup nếu ISR miss)
  static unsigned long lastIrqCheck = 0;
  if (!rx_packet_ready && millis() - lastIrqCheck >= 100) {
    lastIrqCheck = millis();
    uint16_t irq = radio.getIrqStatus();
    if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
      rx_packet_ready = true;
    }
  }

  if (rx_packet_ready) {
    rx_packet_ready = false;
    processIncomingPacket();
  }

  // 2. QoS: Kiểm tra timeout và phát lại (Retransmission)
  if (g_pending_ack.active) {
    if (millis() - g_pending_ack.sent_at >= g_pending_ack.timeout_ms) {
      if (g_pending_ack.retries_left > 0) {
        g_pending_ack.retries_left--;
        g_pending_ack.sent_at = millis();
        g_pending_ack.pkt.sender_id = MY_NODE_ID;
        Serial.printf("[QoS] ACK Timeout msg #%u -> Retrying (%d retries left)...\n",
                      g_pending_ack.pkt.msg_id, g_pending_ack.retries_left);
        executeTransmit(g_pending_ack.pkt);
      } else {
        Serial.printf("[QoS] FAIL: No ACK received for msg #%u!\n",
                      g_pending_ack.pkt.msg_id);
        g_pending_ack.active = false;
      }
    }
  }

  // 2.5. Chat Service: Kiểm tra timeout và thử lại tin nhắn Chat (2000ms / 3 lần)
  tickPendingChat();

  // 3. RX Watchdog (RXPS Watchdog): Giám sát phần cứng và duy trì chế độ nhận
  checkRxWatchdog();

  // 4. Giám sát pin định kỳ mỗi 60 giây
  static unsigned long lastBatteryCheck = 0;
  if (millis() - lastBatteryCheck >= 60000) {
    lastBatteryCheck = millis();
    lora_mesh_checkBatteryProtection();
  }

  // 5. Heartbeat log mỗi 30 giây
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 30000) {
    lastHeartbeat = millis();
    uint32_t sec = millis() / 1000;
    uint32_t h = sec / 3600;
    uint32_t m = (sec / 60) % 60;
    uint32_t s = sec % 60;
    Serial.printf(
        "[STATUS] Up %02lu:%02lu:%02lu | Batt:%.2fV | RX:%lu Relay:%lu TX:%lu Dup:%lu | Routes:%u | WD:%lu | Heap:%luKB\n",
        (unsigned long)h, (unsigned long)m, (unsigned long)s,
        lora_mesh_getBatteryMilliVolts() / 1000.0f,
        (unsigned long)stats_rx_total, (unsigned long)stats_relayed, (unsigned long)stats_tx_sos,
        (unsigned long)stats_dropped_seen, (unsigned int)route_count,
        (unsigned long)stats_rx_wd_resets,
        (unsigned long)(ESP.getFreeHeap() / 1024));
  }

  xSemaphoreGive(loraMutex);
}

// ============================================================
//  Light Sleep — Giảm dòng tiêu thụ MCU xuống ~1.5 - 2mA
// ============================================================
bool lora_mesh_light_sleep(uint32_t timeoutMs) {
#if LORA_LIGHT_SLEEP_ENABLED
  // Không ngủ nếu có gói tin đang chờ hoặc chân DIO1 đang báo mức cao
  if (rx_packet_ready || digitalRead(LORA_DIO1_PIN) == HIGH) {
    return false;
  }

  // Không ngủ dài nếu đang đợi xác nhận gói tin (QoS ACK)
  if (g_pending_ack.active) {
    uint32_t elapsed = millis() - g_pending_ack.sent_at;
    if (elapsed < g_pending_ack.timeout_ms) {
      uint32_t remaining = g_pending_ack.timeout_ms - elapsed;
      if (timeoutMs == 0 || remaining < timeoutMs) {
        timeoutMs = remaining;
      }
    } else {
      return false; // Đã quá hạn ACK, để loop() xử lý retransmit ngay lập tức
    }
  }

  // Không ngủ dài nếu đang đợi xác nhận tin nhắn Chat
  if (g_pending_chat.active) {
    uint32_t elapsed = millis() - g_pending_chat.last_tx_ms;
    if (elapsed < g_pending_chat.timeout_ms) {
      uint32_t remaining = g_pending_chat.timeout_ms - elapsed;
      if (timeoutMs == 0 || remaining < timeoutMs) {
        timeoutMs = remaining;
      }
    } else {
      return false; // Đã quá hạn Chat ACK, để loop() xử lý retransmit ngay lập tức
    }
  }

  // 1. Chờ xuất hết dữ liệu Serial trước khi ngủ để tránh rác ký tự do ngắt xung nhịp
  Serial.flush();

  // 2. Tạm thời gỡ bỏ ngắt GPIO trước khi ngủ để tránh CPU interrupt matrix bị kẹt vòng lặp ngắt mức cao
  detachInterrupt(digitalPinToInterrupt(LORA_DIO1_PIN));

  // 3. Cấu hình nguồn đánh thức bằng RTC EXT1 trên chân DIO1 (mức HIGH khi nhận gói tin)
  esp_sleep_enable_ext1_wakeup(1ULL << LORA_DIO1_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);

  // 4. Cấu hình hẹn giờ định kỳ đánh thức MCU
  if (timeoutMs > 0) {
    esp_sleep_enable_timer_wakeup(timeoutMs * 1000ULL);
  }

  // 5. Vào Light Sleep
  esp_light_sleep_start();

  // 6. Sau khi thức dậy: vô hiệu hóa nguồn đánh thức EXT1
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);

  // 7. Nếu DIO1 ở mức cao, đánh dấu cờ để vòng lặp xử lý ngay
  if (digitalRead(LORA_DIO1_PIN) == HIGH) {
    rx_packet_ready = true;
  }

  // 8. Khôi phục lại ngắt cạnh lên (RISING) cho RadioLib
  radio.setDio1Action(onDio1Interrupt);

  return true;
#else
  if (timeoutMs > 0) {
    delay(timeoutMs);
  }
  return false;
#endif
}
