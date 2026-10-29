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

void lora_mesh_getMac(uint8_t mac[6]) {
  if (g_my_node_id == 0) {
    lora_mesh_initNodeId();
  }
  memcpy(mac, g_device_mac, 6);
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
    Serial.printf("[BATTERY] CRITICAL VOLTAGE (%dmV <= %dmV)! Entering Deep Sleep to protect cell!\n",
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
  Serial.println("[LORA] Hardware reset SX1262...");
  digitalWrite(LORA_RST_PIN, LOW);
  delay(20);
  digitalWrite(LORA_RST_PIN, HIGH);
  delay(50);
  waitBusy(200);
}

static void enterReceiveMode() {
  int16_t state = RADIOLIB_ERR_NONE;

#if LORA_RX_DUTY_CYCLE_ENABLED
  // Bật chế độ thu chu kỳ tiết kiệm điện động (Dynamic RXPS)
  state = radio.startReceiveDutyCycle(g_rxps_rx_us, g_rxps_sleep_us);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] startReceiveDutyCycle err: %d — fallback to continuous RX\n", state);
    state = radio.startReceive();
  }
#else
  state = radio.startReceive();
#endif

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] startReceive FAILED: %d — attempting recovery...\n", state);
    hardResetRadio();
    radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                LORA_TCXO_VOLTAGE, false);
    radio.setDio2AsRfSwitch(true);

#if LORA_RX_DUTY_CYCLE_ENABLED
    state = radio.startReceiveDutyCycle(g_rxps_rx_us, g_rxps_sleep_us);
    if (state != RADIOLIB_ERR_NONE) {
      state = radio.startReceive();
    }
#else
    state = radio.startReceive();
#endif

    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("[LORA] Recovery startReceive ALSO FAILED: %d\n", state);
    } else {
      Serial.println("[LORA] Recovery OK — back in RX mode");
    }
  }
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
    Serial.printf("[LORA] CAD: Kênh bận! Backoff %lums (thử %d/%d)...\n",
                  backoff, retries, LORA_CAD_MAX_RETRIES);
    delay(backoff);
  }
  if (retries >= LORA_CAD_MAX_RETRIES) {
    Serial.println("[LORA] CAD: Kênh vẫn bận sau max retries, buộc phải phát...");
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
    Serial.printf("[LORA] transmit() err: %d\n", state);
  }
  
  // Luôn luôn quay về chế độ RX — kể cả khi TX lỗi
  enterReceiveMode();
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
    Serial.printf("[LORA] Relay TX error: %d\n", state);
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
  ackPkt.sender_id = MY_NODE_ID;
  ackPkt.hop_count = 0;
  ackPkt.payload_len = 0;

  Serial.printf("[LORA] QoS: Đang gửi ACK cho msg #%d tới Node %d...\n", ack_msg_id, dest_id);
  executeTransmit(ackPkt);
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

  enterReceiveMode();

  if (state != RADIOLIB_ERR_NONE) {
    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[LORA] RX Dropped: CRC Mismatch (Nhiễu hoặc sai format)");
    } else {
      Serial.printf("[LORA] readData error: %d\n", state);
    }
    return;
  }

  // 1. Kiểm tra kích thước gói tối thiểu và Magic byte
  if (rxLen > 0 && rxLen < MESH_HEADER_SIZE) {
    Serial.printf("[LORA] RX Dropped: Packet too short (%d bytes < header %d bytes)\n",
                  (int)rxLen, (int)MESH_HEADER_SIZE);
    return;
  }

  if (pkt.magic != MESH_MAGIC) {
    Serial.printf("[LORA] RX Dropped: Invalid magic 0x%02X (Expected 0x%02X)\n",
                  pkt.magic, MESH_MAGIC);
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
  else if (pkt.msg_type == PKT_TYPE_ACK) {
    typeStr = "ACK";
  }
  else if (pkt.msg_type == PKT_TYPE_PING) {
    typeStr = "PING";
  }

  Serial.printf(
      "[LORA] RX %s #%d from Node:%d via:%d | RSSI:%.1f SNR:%.1f | Flags:0x%02X | Path:%s\n",
      typeStr, pkt.msg_id, pkt.src_id, pkt.sender_id, rssi, snr, pkt.flags,
      formatPath(pkt).c_str());

  // Hiển thị nội dung gói tin (hỗ trợ cả nhị phân và văn bản)
  if (pkt.flags & PKT_FLAG_BINARY) {
    if (pkt.payload_len >= sizeof(SosBinaryPayload)) {
      SosBinaryPayload* bin = (SosBinaryPayload*)pkt.payload;
      if (bin->alert_type == ALERT_HEARTBEAT) {
        Serial.printf("[LORA] >> HEARTBEAT TELEMETRY từ Node %d: Pin=%dmV\n",
                      pkt.src_id, bin->battery_mv);
      } else {
        const char* alertName = (bin->alert_type == ALERT_BAWL) ? "BAWL" : "ALERT";
        const char* dirName = (bin->direction == DIR_LEFT) ? "LEFT" :
                              (bin->direction == DIR_RIGHT) ? "RIGHT" : "CENTER";
        Serial.printf("[LORA] >> SOS BINARY: Loại=%s (%d%%) | Hướng=%s | Pin=%dmV\n",
                      alertName, bin->confidence, dirName, bin->battery_mv);
      }
    }
  } else {
    if (pkt.payload_len > 0) {
      Serial.printf("[LORA] >> Payload: %s\n", pkt.payload);
    }
  }

  // 5. QoS: Xử lý khi nhận được gói ACK phản hồi
  if (pkt.flags & PKT_FLAG_IS_ACK) {
    if (g_pending_ack.active && g_pending_ack.pkt.msg_id == pkt.msg_id) {
      uint32_t rtt = millis() - g_pending_ack.sent_at;
      g_pending_ack.active = false;
      Serial.printf("[LORA] QoS: ✓ ACK CONFIRMED cho msg #%d từ Node %d (0x%02X)! (RTT: %lums)\n",
                    pkt.msg_id, pkt.src_id, pkt.src_id, (unsigned long)rtt);
    }
    // Gói ACK đã tới đích cuối thì không relay tiếp
    if (pkt.dest_id == MY_NODE_ID) return;
  }

  // 6. QoS: Phản hồi ACK nếu gói tin yêu cầu
  if (pkt.flags & PKT_FLAG_ACK_REQ) {
    if (pkt.dest_id == MY_NODE_ID || pkt.dest_id == BROADCAST_ID) {
      sendAckPacket(pkt.src_id, pkt.msg_id);
    }
  }

  // Nếu gói tin gửi đích danh cho node này, dừng relay
  if (pkt.dest_id == MY_NODE_ID) {
    return;
  }

  // 7. Hop Limit — Giới hạn số hop (chống flood bão mạng)
  if (pkt.hop_count >= 7) {
    Serial.printf("[LORA] Dropped: Hop limit reached (%d hops)\n", pkt.hop_count);
    return;
  }

  // 8. Loop prevention — chống vòng lặp kín
  for (int i = 0; i < pkt.hop_count && i < MAX_PATH_LEN; i++) {
    if (pkt.path[i] == MY_NODE_ID) {
      Serial.printf("[LORA] Dropped: loop detected (my ID in path)\n");
      return;
    }
  }

  // 9. Cập nhật path + hop
  if (pkt.hop_count < MAX_PATH_LEN) {
    pkt.path[pkt.hop_count] = MY_NODE_ID;
  }
  pkt.hop_count++;
  pkt.sender_id = MY_NODE_ID;

  // 10. Relay
  Serial.printf("[LORA] Relaying %s #%d (hop %d)\n", typeStr, pkt.msg_id,
                pkt.hop_count);

  if (relayPacket(pkt)) {
    stats_relayed++;
    Serial.printf("[LORA] Relay OK!\n");
  }
}

// ============================================================
//  Hàm gửi gói tin nội bộ (hỗ trợ Binary, Text và cờ QoS)
// ============================================================
static bool sendPacketInternal(uint8_t msgType, uint8_t flags, const char *payload,
                               uint8_t payloadLen, uint8_t destId, bool requireAck) {
  if (loraMutex && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[LORA] TX FAILED: Could not take mutex (Radio busy)");
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

  const char *typeStr = (msgType == PKT_TYPE_SOS) ? "SOS" : "DATA";
  Serial.printf("[LORA] TX %s #%d → dest:%d (flags:0x%02X, len:%d)\n",
                typeStr, pkt.msg_id, destId, flags, pkt.payload_len);

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
    Serial.printf("[LORA] TX %s #%d OK!\n", typeStr, pkt.msg_id);
    if (msgType == PKT_TYPE_SOS)
      stats_tx_sos++;
    if (loraMutex) xSemaphoreGive(loraMutex);
    return true;
  } else {
    Serial.printf("[LORA] TX %s FAILED: %d\n", typeStr, state);
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

  Serial.printf("[LORA] Periodic Telemetry Heartbeat: %dmV (ACK=%d)\n", bin.battery_mv, requireAck);
  return sendPacketInternal(PKT_TYPE_DATA, flags, (const char*)&bin, sizeof(bin), GATEWAY_ID, requireAck);
}

bool lora_mesh_sendSOS(const char *payload, uint8_t payloadLen, bool requireAck) {
  uint8_t flags = PKT_FLAG_NONE;
  if (requireAck) flags |= PKT_FLAG_ACK_REQ;
  return sendPacketInternal(PKT_TYPE_SOS, flags, payload, payloadLen, GATEWAY_ID, requireAck);
}

bool lora_mesh_sendData(const char *payload, uint8_t payloadLen, uint8_t destId, bool requireAck) {
  uint8_t flags = PKT_FLAG_NONE;
  if (requireAck) flags |= PKT_FLAG_ACK_REQ;
  return sendPacketInternal(PKT_TYPE_DATA, flags, payload, payloadLen, destId, requireAck);
}

bool lora_mesh_init() {
  Serial.println("[LORA] Initializing SX1262...");

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
  Serial.printf("[LORA] SPI pins: SCK=%d, MISO=%d, MOSI=%d, CS=%d\n",
                LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
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
    Serial.printf("[LORA] Retry with TCXO=%.1fV (err=%d)\n", alt_tcxo, state);
    state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE_LEN,
                        alt_tcxo, false);
  }

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] INIT FAILED: %d\n", state);
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

  Serial.printf(
      "[LORA] SX1262 ready! MAC=%s | Node ID=%d (0x%02X) | %.1fMHz, SF%d, BW%.0f, +%ddBm, Preamble=%d\n",
      lora_mesh_getMacString(), MY_NODE_ID, MY_NODE_ID, LORA_FREQUENCY, LORA_SPREADING_FACTOR, LORA_BANDWIDTH, LORA_TX_POWER, LORA_PREAMBLE_LEN);
#if LORA_RX_DUTY_CYCLE_ENABLED
  Serial.printf("[LORA] Dynamic RXPS: ON (Level %d: RX %luus, Sleep %luus)\n",
                LORA_RXPS_LEVEL, (unsigned long)g_rxps_rx_us, (unsigned long)g_rxps_sleep_us);
#else
  Serial.println("[LORA] Dynamic RXPS: OFF (Continuous RX)");
#endif
#if LORA_CAD_ENABLED
  Serial.println("[LORA] CAD Collision Avoidance: ON");
#endif
  Serial.printf("[LORA] Battery Voltage: %dmV\n", lora_mesh_getBatteryMilliVolts());
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
        Serial.printf("[LORA] QoS: ACK Timeout! Retransmitting msg #%d (còn %d lần thử)...\n",
                      g_pending_ack.pkt.msg_id, g_pending_ack.retries_left);
        executeTransmit(g_pending_ack.pkt);
      } else {
        Serial.printf("[LORA] QoS: Hết số lần thử! Không nhận được ACK cho msg #%d!\n",
                      g_pending_ack.pkt.msg_id);
        g_pending_ack.active = false;
      }
    }
  }

  // 3. Giám sát pin định kỳ mỗi 60 giây
  static unsigned long lastBatteryCheck = 0;
  if (millis() - lastBatteryCheck >= 60000) {
    lastBatteryCheck = millis();
    lora_mesh_checkBatteryProtection();
  }

  // 4. Heartbeat log mỗi 30 giây
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 30000) {
    lastHeartbeat = millis();
    Serial.printf(
        "[LORA] Alive | Up:%lus | RX:%lu Relayed:%lu SOS_TX:%lu Dup:%lu | Batt:%dmV | Routes:%d\n",
        millis() / 1000, stats_rx_total, stats_relayed, stats_tx_sos,
        stats_dropped_seen, lora_mesh_getBatteryMilliVolts(), route_count);
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
