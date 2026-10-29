# Phân Tích Cơ Chế LoRa Mesh & Hướng Dẫn Tích Hợp Vào Repeater

Repository `mesh_core` cung cấp giải pháp mạng lưới (Mesh Network) phi tập trung qua sóng LoRa. Dưới đây là phân tích luồng hoạt động cơ bản và hướng dẫn chi tiết để bạn tích hợp trực tiếp cơ chế mesh này vào một Repeater (trạm chuyển tiếp) tùy chỉnh, sử dụng cấu trúc gói tin tĩnh mà không cần các thư viện phức tạp.

## 1. Cơ Chế Hoạt Động Cốt Lõi (Mesh Routing Mechanism)

Bất kỳ một trạm Repeater nào trong mạng LoRa Mesh đều phải đối mặt với hai vấn đề lớn: **Bão mạng (Broadcast Storm)** và **Đụng độ sóng (Collision)**. Hệ thống giải quyết các vấn đề này thông qua 4 cơ chế cốt lõi:

1. **Deduplication (Triệt tiêu trùng lặp)**: 
   Mỗi gói tin được đánh dấu bằng `src_id` (ID nguồn gốc) và `msg_id` (ID định danh gói). Repeater sẽ lưu một bộ đệm (Seen Cache) để ghi nhớ các gói đã nhận trong một khoảng thời gian (VD: 15 giây). Nếu nhận lại gói đã có trong Cache (do trạm khác vô tình chuyển tiếp ngược lại), nó sẽ bị bỏ qua.
2. **Time-To-Live (TTL - Giới hạn số chặng)**: 
   Sử dụng thông số `hop_count` và `max_hops`. Mỗi khi gói tin nhảy qua một trạm, `hop_count` tăng lên 1. Nếu `hop_count >= max_hops`, gói tin bị loại bỏ để tránh việc nó trôi nổi vô tận giữa các trạm.
3. **Loop Prevention (Chống lặp vòng kín)**:
   Gói tin mang theo một mảng `path` chứa danh sách ID của các trạm nó đã đi qua. Trước khi quyết định phát tiếp, Repeater kiểm tra xem ID của chính mình đã nằm trong `path` chưa. Nếu có, tức là gói đang đi thành một vòng khép kín -> Hủy gói.
4. **Collision Avoidance (Tránh đụng độ / Backoff Delay)**:
   Để tránh việc nhiều Repeater cùng phát lại một lúc khi vừa nhận được cùng một sóng (gây nhiễu đè nhau), một độ trễ ngẫu nhiên (Random Jitter), hoặc phân khe thời gian (Time-slot) dựa trên ID trạm, sẽ được tính toán làm thời gian chờ trước khi ra lệnh Transmit (TX).

---

## 2. Luồng Hoạt Động (Workflow) Của Một Trạm Repeater

Quy trình xử lý tuần tự trên Repeater mỗi khi module LoRa bắt được sóng (thường thông qua Ngắt RX_DONE):

```mermaid
flowchart TD
    RX[Nhận gói tin từ module LoRa] --> CheckMagic{Kiểm tra Byte định dạng?}
    CheckMagic -- Sai --> Drop1[Hủy gói]
    CheckMagic -- Đúng --> CheckSelf{Gói do mình tự gửi ban đầu?}
    CheckSelf -- Phải --> Drop2[Hủy gói]
    CheckSelf -- Không --> CheckDup{Đã có trong Seen Cache?}
    CheckDup -- Có --> Drop3[Hủy gói (Trùng lặp)]
    CheckDup -- Không --> AddCache[Lưu vết vào Seen Cache]
    AddCache --> CheckTTL{hop_count >= max_hops?}
    CheckTTL -- Đúng --> Drop4[Hủy gói (Quá số Hops)]
    CheckTTL -- Sai --> CheckLoop{ID trạm đã có trong path[]?}
    CheckLoop -- Có --> Drop5[Hủy gói (Lặp vòng)]
    CheckLoop -- Không --> Modify[Cập nhật path[] và hop_count]
    Modify --> Backoff[Chờ ngẫu nhiên / Slotting Delay]
    Backoff --> TX[Phát chuyển tiếp - TX]
    TX --> Listen[Quay lại chế độ Lắng nghe RX]
```

---

## 3. Hướng Dẫn Code Tích Hợp Cơ Chế Vào Repeater

Dưới đây là mã mẫu C/C++ để xây dựng logic định tuyến chuyển tiếp, giả định bạn dùng thư viện `RadioLib`.

### Bước 1: Định nghĩa cấu trúc gói tin

Gói tin (Payload) phải đồng nhất giữa mọi thiết bị (Node, Gateway và Repeater).
```cpp
#define MESH_MAGIC 0xAA

#pragma pack(push, 1)
struct MeshPacket {
  uint8_t  magic;          // 0xAA (Dấu hiệu nhận biết gói Mesh của bạn)
  uint16_t msg_id;         // ID tuần tự để chống trùng
  uint8_t  msg_type;       // Phân loại: 1=DATA, 2=SOS, 3=ACK...
  uint8_t  src_id;         // ID trạm gửi gốc ban đầu
  uint8_t  dest_id;        // ID trạm đích đến
  uint8_t  sender_id;      // ID trạm vừa phát ra sóng (Hop gần nhất)
  uint8_t  hop_count;      // Số chặng đã nhảy qua
  uint8_t  max_hops;       // Giới hạn số chặng (TTL)
  uint8_t  path[4];        // Mảng lưu ID các trạm Repeater đã đi qua
  uint8_t  payload_len;    // Chiều dài dữ liệu payload
  char     payload[48];    // Dữ liệu nội dung
};
#pragma pack(pop)
```

### Bước 2: Thiết lập Bộ nhớ đệm (Seen Cache)

Cơ chế này đảm bảo trạm không phát lại 1 gói 2 lần.

```cpp
#define SEEN_CACHE_SIZE 32
struct SeenPacket {
  uint8_t  src_id;
  uint16_t msg_id;
  uint32_t timestamp;
};
SeenPacket seen_cache[SEEN_CACHE_SIZE];
uint8_t seen_idx = 0;

// Hàm kiểm tra xem gói tin này đã được xử lý gần đây chưa
bool isPacketSeen(uint8_t src_id, uint16_t msg_id) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seen_cache[i].src_id == src_id && seen_cache[i].msg_id == msg_id) {
      if (now - seen_cache[i].timestamp < 15000) { // Timeout 15 giây
        return true;
      }
    }
  }
  return false;
}

// Hàm ghi nhớ gói tin vào lịch sử
void markPacketSeen(uint8_t src_id, uint16_t msg_id) {
  seen_cache[seen_idx] = {src_id, msg_id, millis()};
  seen_idx = (seen_idx + 1) % SEEN_CACHE_SIZE;
}
```

### Bước 3: Hàm xử lý luồng Logic cốt lõi

Khi module LoRa nhận được tín hiệu (dưới dạng Ngắt ngầm), bạn đưa mảng byte dữ liệu đọc được vào hàm phân tích này.

```cpp
uint8_t my_id = 2; // ID của trạm Repeater hiện tại

void processIncomingPacket() {
  MeshPacket pkt;
  int state = radio.readData((uint8_t*)&pkt, sizeof(pkt));
  
  // Kiểm tra lỗi đọc và lọc Magic Byte
  if (state != RADIOLIB_ERR_NONE || pkt.magic != MESH_MAGIC) {
    radio.startReceive(); // Nếu không phải gói Mesh -> Bỏ qua
    return;
  }

  // 1. Lọc gói do chính mình gửi đi (Tiếng vọng)
  if (pkt.src_id == my_id) return;

  // 2. Triệt tiêu trùng lặp (Deduplication)
  if (isPacketSeen(pkt.src_id, pkt.msg_id)) return;
  markPacketSeen(pkt.src_id, pkt.msg_id);

  // 3. Kiểm tra TTL
  if (pkt.hop_count >= pkt.max_hops) return;

  // 4. Chống lặp vòng kín (Loop Prevention)
  for (int i = 0; i < pkt.hop_count && i < 4; i++) {
    if (pkt.path[i] == my_id) return; // Trạm này đã từng chuyển tiếp gói này -> Hủy
  }

  // 5. Cập nhật thông tin gói trước khi Forward
  if (pkt.hop_count < 4) {
    pkt.path[pkt.hop_count] = my_id; // Đóng dấu ID vào Path
  }
  pkt.hop_count++;                   // Tăng số chặng
  pkt.sender_id = my_id;             // Đánh dấu mình là người vừa phát sóng

  // 6. Tránh đụng độ sóng (CSMA/Backoff Delay)
  // Tính toán thời gian chờ ngẫu nhiên kết hợp chia Slot dựa vào ID trạm
  uint32_t base_slot = (my_id % 2 == 0) ? 60 : 260; 
  uint32_t jitter = base_slot + random(10, 90);
  delay(jitter);

  // 7. Thực thi việc Chuyển tiếp (Relay)
  radio.transmit((uint8_t*)&pkt, sizeof(pkt));
  
  // 8. Đặt module trở lại trạng thái đón sóng tiếp theo
  radio.startReceive();
}
```
