#ifndef LORA_MESH_H
#define LORA_MESH_H

#include <Arduino.h>
#include <stddef.h>

// ============================================================
//  Mesh Packet Protocol
// ============================================================
#define MESH_MAGIC       0xAA
#define MAX_PATH_LEN     8     // Kích thước mảng path[] cho loop prevention
#define PKT_TYPE_DATA    1
#define PKT_TYPE_SOS     2
#define PKT_TYPE_ACK     3
#define PKT_TYPE_PING    4

// --- Packet Flags (QoS & Định dạng) ---
#define PKT_FLAG_NONE     0x00
#define PKT_FLAG_ACK_REQ  0x01 // Yêu cầu trả lời ACK từ node đích (QoS)
#define PKT_FLAG_IS_ACK   0x02 // Là gói phản hồi ACK
#define PKT_FLAG_BINARY   0x04 // Payload là định dạng nhị phân siêu gọn

// ============================================================
//  Định dạng Nhị phân Siêu gọn cho Cảnh báo SOS (Chỉ 5 bytes!)
// ============================================================
enum AlertType : uint8_t {
  ALERT_NONE      = 0, // Không có sự kiện
  ALERT_HEARTBEAT = 0, // Gói đo lường pin & nhịp tim định kỳ
  ALERT_BAWL      = 1, // Tiếng kêu cứu / la hét (KWS detected)
  ALERT_SCREAM    = 2,
  ALERT_HELP      = 3,
  ALERT_TEST      = 99
};

enum DirectionType : uint8_t {
  DIR_CENTER  = 0,
  DIR_LEFT    = 1,
  DIR_RIGHT   = 2,
  DIR_UNKNOWN = 3
};

#pragma pack(push, 1)
struct SosBinaryPayload {
  uint8_t  alert_type;   // AlertType (1 byte)
  uint8_t  confidence;   // Độ tin cậy 0 - 100% (1 byte)
  uint8_t  direction;    // DirectionType (1 byte)
  uint16_t battery_mv;   // Điện áp pin đo được (2 bytes)
};

struct MeshPacket {
  uint8_t  magic;          // 0xAA (Packet identifier)
  uint16_t msg_id;         // Unique sequence ID
  uint8_t  msg_type;       // 1: DATA, 2: SOS, 3: ACK, 4: PING
  uint8_t  flags;          // PKT_FLAG_* (ACK_REQ, IS_ACK, BINARY)
  uint8_t  src_id;         // Original transmitter ID
  uint8_t  dest_id;        // Final destination ID (0 = Gateway, 255 = Broadcast)
  uint8_t  sender_id;      // Immediate hop transmitter ID
  uint8_t  hop_count;      // Hops traversed so far (starts at 0)
  uint8_t  path[MAX_PATH_LEN]; // Route path trace (supports up to 8 hops)
  uint8_t  payload_len;    // Length of payload
  char     payload[128];   // Payload data (string hoặc SosBinaryPayload)
};
#pragma pack(pop)

// Kích thước Header cố định (không tính payload)
#define MESH_HEADER_SIZE (offsetof(MeshPacket, payload))

// ============================================================
//  API
// ============================================================

/// Khởi tạo Node ID từ eFuse MAC của ESP32
void lora_mesh_initNodeId();

/// Lấy Node ID của thiết bị (sinh từ byte cuối của MAC eFuse, giá trị 1 - 254)
uint8_t lora_mesh_getNodeId();

/// Lấy chuỗi biểu diễn địa chỉ MAC eFuse (vd: "34:85:18:9A:BC:2A")
const char* lora_mesh_getMacString();

/// Lấy mảng 6 byte địa chỉ MAC eFuse
void lora_mesh_getMac(uint8_t mac[6]);

/// Khởi tạo SX1262 + vào chế độ nhận
/// @return true nếu thành công
bool lora_mesh_init();

/// Gọi trong loop() chính — xử lý gói tin đến + relay + QoS retry timeout
void lora_mesh_loop();

/// Gửi gói SOS dạng nhị phân siêu gọn (kèm telemetry pin và yêu cầu ACK)
/// @param alert Loại cảnh báo (ALERT_BAWL, ...)
/// @param confidence Điểm tin cậy (0-100%)
/// @param dir Hướng âm thanh từ stereo mic
/// @param requireAck Có kích hoạt cơ chế chờ ACK và tự động retry hay không
/// @param destId Node nhận đích (0: GATEWAY_ID, 255: BROADCAST_ID, hoặc ID node cụ thể)
/// @return true nếu phát thành công
bool lora_mesh_sendSosBinary(AlertType alert, uint8_t confidence, DirectionType dir, bool requireAck = true, uint8_t destId = 0);

/// Gửi gói Heartbeat / Telemetry pin định kỳ (Payload 5 bytes siêu gọn)
/// @param requireAck Có yêu cầu Gateway phản hồi ACK hay không (mặc định false để tiết kiệm sóng)
/// @return true nếu gửi thành công
bool lora_mesh_sendHeartbeat(bool requireAck = false);

/// Gửi gói SOS (dạng chuỗi text/JSON truyền thống)
/// @param payload Nội dung cần gửi
/// @param payloadLen Chiều dài payload
/// @param requireAck Có yêu cầu ACK hay không
/// @return true nếu phát thành công
bool lora_mesh_sendSOS(const char* payload, uint8_t payloadLen, bool requireAck = true);

/// Gửi gói DATA tổng quát
bool lora_mesh_sendData(const char* payload, uint8_t payloadLen, uint8_t destId = 0, bool requireAck = false);

/// Đưa ESP32 vào chế độ Light Sleep khi rảnh rỗi (thức dậy bằng ngắt DIO1 hoặc timer)
/// @param timeoutMs Thời gian tối đa ngủ trước khi tự động thức dậy (ms)
/// @return true nếu đã ngủ và thức dậy thành công
bool lora_mesh_light_sleep(uint32_t timeoutMs);

/// Kiểm tra kênh có bận không bằng phần cứng CAD (Channel Activity Detection)
/// @return true nếu phát hiện có tín hiệu LoRa trên kênh
bool lora_mesh_isChannelActive();

/// Đọc điện áp pin hiện tại (mV)
uint16_t lora_mesh_getBatteryMilliVolts();

/// Kiểm tra bảo vệ pin yếu (tự động vào Deep Sleep nếu < 3000mV)
void lora_mesh_checkBatteryProtection();

/// Hàm tính toán động chu kỳ RXPS (MeshCore-style Dynamic Calculation)
void lora_mesh_calcRxpsTimings(uint8_t level, uint8_t sf, float bw, uint8_t preamble, uint32_t* rx_us, uint32_t* sleep_us);

#endif // LORA_MESH_H
