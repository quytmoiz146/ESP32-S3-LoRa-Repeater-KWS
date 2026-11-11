# 📘 Hướng Dẫn Tích Hợp Thiết Bị Node Cầm Tay / Balo (LoRa Mesh + BLE)

Tài liệu này cung cấp hướng dẫn kỹ thuật và **Prompt chuẩn cho AI** giúp các kỹ sư nhanh chóng tích hợp tính năng mạng lưới LoRa Mesh và kết nối di động Bluetooth Low Energy (BLE) vào bo mạch phần cứng và firmware của thiết bị Node hiện trường.

---

## 1. Vai Trò Thiết Bị & Luồng Dữ Liệu

Thiết bị **Node Cầm tay / Balo** phục vụ việc liên lạc khẩn cấp hai chiều cho các cá nhân hoặc đội tuần tra ngoài thực địa:

```
+------------------------+          BLE 4.2 / 5.0 (NUS)          +------------------------+
|   Điện Thoại Di Động   | <==================================> |    ESP32-S3 (Node)     |
|   (App Cứu Hộ / Chat)  |   Nordic UART Service (UUID 6E40...) |   (ID tự động theo MAC)|
+------------------------+                                      +-----------+------------+
                                                                            |
                                                                       SPI  | (HSPI / FSPI)
                                                                            v
                                                                +------------------------+
                                                                | Module LoRa SX1262     |
                                                                | (Tần số 921.4 MHz SF9) |
                                                                +-----------+------------+
                                                                            |
                                                                       LoRa | Sóng RF
                                                                            v
                                                                [ Mạng Lưới LoRa Mesh ]
                                                            (Trạm Repeater -> Gateway Trung Tâm)
```

* **Kết nối tầm gần (Smartphone)**: Dùng dịch vụ BLE tiêu chuẩn **Nordic UART Service (NUS)**.
* **Kết nối tầm xa (LoRa Mesh)**: Hoạt động ở tần số 921.4 MHz, công suất phát cực đại +22dBm, tự động tìm đường qua các trạm lặp (Repeater) về Gateway (ID: 0).
* **Tự động nhận diện vai trò (Role Flag `0x08`)**: Tự động đánh dấu cờ `PKT_FLAG_FROM_NODE` trong header gói tin để kích hoạt còi tại trạm Repeater đầu tiên và thông báo vai trò `NODE` về Gateway.
* **Độ tin cậy cao (Stop-and-Wait ARQ)**: Tự động gửi lại tối đa 3 lần (timeout 2000ms), trả về trạng thái rõ ràng (`SENDING`, `DELIVERED`, `FAILED`).
* **Không dùng phím tắt console**: Node hoạt động thuần túy qua giao thức chuẩn hóa (BLE / Serial command) để đảm bảo tính ổn định.

### 1.1. Cơ Chế Nhận Diện Vai Trò & Phối Hợp Mạng (Device Role & Network Interaction)

Khi tích hợp firmware cho thiết bị Node, macro định danh vai trò `-D ROLE_NODE` đóng vai trò then chốt trong việc đồng bộ hóa hành vi toàn mạng:

1. **Đánh dấu cờ Header 0-overhead**:
   * Khi `ROLE_NODE` được định nghĩa, mọi gói tin gốc do thiết bị này phát đi (SOS, Chat, ACK, Heartbeat) sẽ tự động bật cờ:
     ```cpp
     #define PKT_FLAG_FROM_NODE 0x08 // Bit 3 trong trường flags của MeshPacket
     ```
2. **Kích hoạt còi tại trạm Repeater chặng đầu tiên**:
   * Khi Node phát bản tin khẩn cấp SOS: Trạm Repeater đầu tiên tiếp sóng trực tiếp (`hop_count == 0`) và được chọn làm chặng chuyển tiếp kế tiếp (`next_hop`) sẽ **ngay lập tức hú còi cảnh báo 5 giây tại chỗ** để xác nhận đã tiếp nhận tín hiệu cứu hộ thành công.
   * Các trạm Repeater bị `[SKIP]` hoặc các trạm chuyển tiếp ở chặng 2, chặng 3 xa hơn sẽ không hú còi để tránh gây nhiễu âm thanh diện rộng.
3. **Phân loại nguồn tin trên Gateway & Bản đồ tác chiến**:
   * Nhờ cờ `PKT_FLAG_FROM_NODE`, Gateway trung tâm luôn giải mã chính xác nguồn phát là `"role": "NODE"` (ba lô tuần tra) thay vì trạm cố định `"role": "REPEATER"`, giúp Dashboard hiển thị đúng biểu tượng trên bản đồ.

---

## 2. Cấu Hình Phần Cứng & Giao Tiếp SX1262

> [!IMPORTANT]
> **Các chân GPIO kết nối với module LoRa SX1262 KHÔNG CỐ ĐỊNH**. Mỗi dòng bo mạch phần cứng tự thiết kế sẽ có sơ đồ chân GPIO khác nhau.
> 
> Bạn có thể tùy biến các chân này trực tiếp trong file cấu hình (ví dụ: `include/config.h`) hoặc định nghĩa qua `build_flags` trong `platformio.ini`.

### Bảng sơ đồ chân Module LoRa SX1262 trên Node (Đồng bộ Gateway):

| Tên chân Module SX1262 | Chân ESP32-S3 | Chức năng kỹ thuật | Ghi chú cấu hình |
| :--- | :---: | :--- | :--- |
| **VCC** | **3.3V** | Nguồn cấp cho chip LoRa | Bắt buộc 3.3V, cấp đủ dòng khi phát đỉnh (120mA) |
| **GND** | **GND** | Mass nối đất chung | Nối chung mass toàn hệ thống |
| **SCK** | **GPIO 40** | SPI Serial Clock | Xung nhịp truyền dữ liệu SPI |
| **MOSI** | **GPIO 39** | SPI Master Out Slave In | Đường truyền dữ liệu ra SX1262 |
| **MISO** | **GPIO 41** | SPI Master In Slave Out | Đường nhận dữ liệu từ SX1262 |
| **NSS / CS** | **GPIO 42** | SPI Chip Select | Tích cực mức LOW |
| **RESET** | **GPIO 7** | Reset chip phần cứng | Tích cực mức LOW |
| **BUSY** | **GPIO 48** | Báo bận phần cứng SX1262 | Khi HIGH: Chip đang bận |
| **DIO1** | **GPIO 2** | Ngắt thu/phát gói tin LoRa | Kích hoạt ngắt cạnh lên (RISING) |
| **DIO3** | **GPIO 1** | Tín hiệu điều khiển/giám sát DIO3 | Tín hiệu phụ SX1262 |

### Cấu hình mã nguồn trong `include/config_node.h`:
```cpp
// --- Sơ đồ chân LoRa SX1262 trên Node (Đồng bộ với Gateway) ---
#define LORA_SCK_PIN    40
#define LORA_MOSI_PIN   39
#define LORA_MISO_PIN   41
#define LORA_CS_PIN     42
#define LORA_RST_PIN    7
#define LORA_BUSY_PIN   48
#define LORA_DIO1_PIN   2
#define LORA_DIO3_PIN   1

// Ngoại vi
#define LED             -1   // Tránh xung đột với LORA_BUSY (GPIO 48)
#define ALARM_PIN       -1
#define PIN_VBAT_READ   -1   // GPIO 1 dùng cho LORA_DIO3
```

---

## 3. Cấu Hình Môi Trường Firmware (PlatformIO)

Trong `platformio.ini`, môi trường nạp cho thiết bị Node được cấu hình độc lập:

```ini
[env:node_ble]
upload_port = COM15       ; Cổng COM nạp thực tế của bạn
monitor_port = COM15
upload_speed = 921600
build_unflags = ${usb_ttl.build_unflags}
build_flags = 
	${env.build_flags} 
	${usb_ttl.build_flags}
	-D ROLE_NODE

lib_deps =
	${env.lib_deps}
	h2zero/NimBLE-Arduino @ ^1.4.1
	bblanchon/ArduinoJson @ ^6.21.3

build_src_filter =
	+<main_node.cpp>
	+<lora_mesh/>
	+<ble/>
	-<main_repeater_full.cpp>
	-<main_repeater_simple.cpp>
	-<main_gateway.cpp>
	-<capture/>
	-<keyword_spot/>
	-<stereo_process/>
	-<tier0/>
	-<ringbuffer/>
```

---

## 4. Giao Thức BLE Nordic UART Service (NUS) & Dữ Liệu

### 4.1. Thông số UUID BLE chuẩn:
* **Tên thiết bị quảng bá**: `SOS-Node-<HexID>` (Ví dụ: `SOS-Node-A2`).
* **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
* **RX Characteristic UUID (App ➔ ESP32)**: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` (Thuộc tính: `WRITE`)
* **TX Characteristic UUID (ESP32 ➔ App)**: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` (Thuộc tính: `NOTIFY`)

### 4.2. Cú pháp gửi tin từ Mobile App:
```text
CHAT:<client_id>:<target_node>:<utf8_text>\n
```
* **Ví dụ**: `CHAT:1001:0:Đội tuần tra số 2 đã đến điểm tập kết\n` (Gửi về Gateway `0`).

### 4.3. Định dạng sự kiện phản hồi lên App (TX Notify):
* **Khi bắt đầu phát sóng**:
  ```json
  {"type":"CHAT_STATUS","client_id":1001,"msg_id":2415,"status":"SENDING"}
  ```
* **Khi nhận xác nhận ACK từ đích cuối**:
  ```json
  {"type":"CHAT_STATUS","client_id":1001,"msg_id":2415,"status":"DELIVERED"}
  ```
* **Khi hết số lần thử lại (3 lần)**:
  ```json
  {"type":"CHAT_STATUS","client_id":1001,"msg_id":2415,"status":"FAILED","reason":"NO_ACK"}
  ```
* **Khi có tin nhắn đến từ mạng Mesh**:
  ```json
  {"type":"CHAT","node_id":"NODE162","src":0,"dest":162,"text":"Chỉ huy nhận rõ, duy trì vị trí","msg_id":128,"hops":2,"rssi":-78.5,"snr":8.2}
  ```

---

## 5. Hướng Dẫn Kiểm Thử Bằng Ứng Dụng Di Động

1. Cài đặt app **Serial Bluetooth Terminal** (Android) hoặc **nRF Connect for Mobile** (Android/iOS).
2. Quét BLE và kết nối với thiết bị có tên dạng `SOS-Node-XX`.
3. Gửi lệnh: `CHAT:1:0:Xin chao Gateway\n`
4. Quan sát chuỗi JSON trạng thái `CHAT_STATUS` báo `SENDING` rồi `DELIVERED` khi có Gateway trả ACK.

---

## 6. 🤖 Prompt Chuẩn Cho AI Để Tích Hợp Vào Dự Án Node

Sao chép toàn bộ khối nội dung bên dưới và dán vào trợ lý AI (ChatGPT, Claude, Antigravity, Copilot) để AI tự động tích hợp mã nguồn vào dự án của bạn:

```markdown
Hãy giúp tôi tích hợp chức năng "Node Cầm Tay / Balo (LoRa Mesh + BLE Chat)" vào dự án ESP32 hiện tại của tôi.

Dưới đây là các yêu cầu và đặc tả kỹ thuật cần tuân thủ:

1. PHẦN CỨNG:
- Vi điều khiển: ESP32 hoặc ESP32-S3.
- Module LoRa: SX1262 (sử dụng thư viện RadioLib).
- Giao tiếp: SPI với các chân CS, MOSI, MISO, SCK, RST, BUSY, DIO1 (hãy hỏi tôi sơ đồ chân GPIO của bo mạch tôi đang dùng nếu trong mã nguồn chưa có).
- Tham số RF: Tần số 921.4 MHz, SF 9, BW 125.0 kHz, CR 4/7, TX Power +22 dBm, Preamble 16 symbols.

2. KẾT NỐI BLUETOOTH LOW ENERGY (BLE):
- Sử dụng thư viện: NimBLE-Arduino (nhẹ và tiết kiệm RAM).
- Dịch vụ: Nordic UART Service (NUS).
  + Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  + RX Characteristic UUID (Nhận lệnh từ Mobile): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E (WRITE)
  + TX Characteristic UUID (Báo tin lên Mobile): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E (NOTIFY)
- Tên thiết bị quảng bá: "SOS-Node-<HEX_ID>" (HEX_ID lấy từ byte cuối MAC phần cứng).

3. ĐỊNH DẠNG GIAO THỨC CHAT:
- Lệnh từ Mobile App gửi xuống qua BLE RX hoặc Serial:
  Cú pháp: CHAT:<client_id>:<target_node>:<utf8_text>\n
  (target_node = 0 là Gateway chỉ huy; 255 là Broadcast).
- Sự kiện phản hồi lên Mobile App qua BLE TX Notify (định dạng NDJSON kết thúc bằng \n):
  + Trạng thái phát: {"type":"CHAT_STATUS","client_id":<id>,"msg_id":<num>,"status":"SENDING"}
  + Giao thành công (nhận ACK đích): {"type":"CHAT_STATUS","client_id":<id>,"msg_id":<num>,"status":"DELIVERED"}
  + Thất bại sau 3 lần thử: {"type":"CHAT_STATUS","client_id":<id>,"msg_id":<num>,"status":"FAILED","reason":"NO_ACK"}
  + Khi nhận tin từ mạng: {"type":"CHAT","node_id":"NODE<id>","src":<id>,"dest":<id>,"text":"<utf8>","msg_id":<num>,"hops":<num>,"rssi":<val>,"snr":<val>}

4. YÊU CẦU TÍCH HỢP MÃ NGUỒN:
- Cấu hình vai trò qua macro biên dịch: `-D ROLE_NODE`.
- Khi phát bất kỳ gói tin nào qua mạng LoRa Mesh (SOS, Chat, ACK, Heartbeat), tự động gắn cờ `PKT_FLAG_FROM_NODE = 0x08` trong trường `flags` của header gói tin `MeshPacket` để kích hoạt còi tại Repeater chặng đầu tiên và thông báo vai trò "NODE" về Gateway.
- Không sử dụng các phím tắt console ký tự đơn lẻ.
- Tạo mã nguồn gọn gàng, tách lớp rõ ràng (driver LoRa Mesh riêng, BLE NUS link riêng).
- Đảm bảo trong loop() gọi định kỳ lora_mesh_loop() và xử lý đệm lệnh từ BLE/Serial mà không bị nghẽn (non-blocking).

Hãy xem xét cấu trúc dự án của tôi và hướng dẫn tôi các bước sửa file, thêm thư viện vào platformio.ini và tích hợp mã nguồn tương ứng.
```
