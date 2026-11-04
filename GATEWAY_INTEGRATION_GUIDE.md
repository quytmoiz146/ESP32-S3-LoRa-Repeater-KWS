# 🏛️ Hướng Dẫn Tích Hợp Trạm Cổng Chỉ Huy Gateway (LoRa Mesh + PC NDJSON)

Tài liệu này cung cấp hướng dẫn kỹ thuật và **Prompt chuẩn cho AI** giúp các kỹ sư nhanh chóng tích hợp tính năng Trạm Cổng Chỉ Huy Gateway (Role Gateway) vào bo mạch phần cứng và phần mềm máy tính điều khiển trung tâm.

---

## 1. Vai Trò Trạm Chỉ Huy & Luồng Dữ Liệu

Trạm **Gateway** đóng vai trò là đầu mối tiếp nhận, giải mã và phản hồi tập trung toàn bộ dữ liệu từ hiện trường về máy tính chỉ huy:

```
+-------------------------------------------------------------+
|                 MÁY TÍNH TRUNG TÂM CHỈ HUY                   |
|  (Phần Mềm Quản Trị / Bản Đồ Giám Sát Tác Chiến / Web GIS)   |
+------------------------------+------------------------------+
                               ^
            USB Serial (115200)| Chuẩn giao tiếp NDJSON hai chiều
                               v
+------------------------------+------------------------------+
|                ESP32-S3 GATEWAY (ID Cố Định: 0)              |
|          Định tuyến Mesh, Quản lý ACK, Giải mã gói tin       |
+------------------------------+------------------------------+
                               | Giao tiếp SPI (HSPI / FSPI)
                               v
+-------------------------------------------------------------+
|              MODULE LORA SX1262 (+22dBm, 921.4MHz)          |
|      Anten độ lợi cao (Omni sợi thủy tinh / Yagi định hướng) |
+------------------------------+------------------------------+
                               | Sóng vô tuyến LoRa Mesh
                               v
       [ Mạng Lưới Phủ Sóng Rừng Núi / Trạm Lặp Repeater ]
       <--- SOS KWS / Chat / Heartbeat / Ping từ các Đội Tuần Tra
```

### Đặc tính chính của Gateway:
* **Địa chỉ cố định (ID = 0)**: Toàn bộ gói tin khẩn cấp SOS và báo cáo điện áp pin Telemetry đều mặc định hướng đích về Gateway ID `0`.
* **Giao tiếp PC qua USB Serial**: Tốc độ baud **115200**, định dạng dữ liệu chuẩn **NDJSON (Newline Delimited JSON)** giúp các ứng dụng máy tính (Python, Node.js, C#, Web Serial API) dễ dàng phân tích dữ liệu mà không sợ lỗi framing.
* **Hỗ trợ Chat hai chiều**: Chỉ huy có thể soạn tin nhắn gửi đích danh tới từng Node/Balo hoặc gửi Broadcast cho toàn bộ lực lượng ngoài hiện trường.
* **Hoạt động thuần giao thức**: Loại bỏ hoàn toàn các phím bấm console đơn ký tự, đảm bảo không sinh dữ liệu rác lên kênh truyền máy tính.

---

## 2. Cấu Hình Phần Cứng & Giao Tiếp SX1262

> [!IMPORTANT]
> **Các chân GPIO kết nối với module LoRa SX1262 KHÔNG CỐ ĐỊNH**. Mỗi dòng bo mạch phần cứng tự thiết kế sẽ có sơ đồ chân GPIO khác nhau.
> 
> Bạn có thể tùy biến các chân này trực tiếp trong file cấu hình (ví dụ: `include/config.h`) hoặc định nghĩa qua `build_flags` trong `platformio.ini`.

### Bảng sơ đồ chân Module LoRa SX1262 trên Gateway:

| Tên chân Module SX1262 | Chân ESP32-S3 | Chức năng kỹ thuật | Ghi chú cấu hình |
| :--- | :---: | :--- | :--- |
| **VCC** | **3.3V** | Nguồn cấp cho chip LoRa | Cấp nguồn 3.3V ổn định, có tụ lọc chống sụt áp |
| **GND** | **GND** | Mass nối đất chung | Nối chung mass toàn hệ thống |
| **SCK** | **GPIO 40** | SPI Serial Clock | Xung nhịp truyền dữ liệu SPI |
| **MOSI** | **GPIO 39** | SPI Master Out Slave In | Đường truyền dữ liệu ra SX1262 |
| **MISO** | **GPIO 41** | SPI Master In Slave Out | Đường nhận dữ liệu từ SX1262 |
| **NSS / CS** | **GPIO 42** | SPI Chip Select | Tích cực mức LOW |
| **RESET** | **GPIO 7** | Reset chip phần cứng | Tích cực mức LOW |
| **BUSY** | **GPIO 48** | Báo bận phần cứng SX1262 | Khi HIGH: Chip đang bận |
| **DIO1** | **GPIO 2** | Ngắt thu/phát gói tin LoRa | Kích hoạt ngắt cạnh lên (RISING) |
| **DIO3** | **GPIO 1** | Tín hiệu điều khiển/giám sát DIO3 | Tín hiệu phụ SX1262 |

### Cấu hình mã nguồn trong `include/config.h`:
```cpp
// --- Sơ đồ chân LoRa SX1262 riêng cho Trạm Cổng Chỉ Huy (Gateway) ---
#if defined(ROLE_GATEWAY)
#define LORA_SCK_PIN   40
#define LORA_MOSI_PIN  39
#define LORA_MISO_PIN  41
#define LORA_CS_PIN    42
#define LORA_RST_PIN   7
#define LORA_BUSY_PIN  48
#define LORA_DIO1_PIN  2
#define LORA_DIO3_PIN  1

// Gateway cắm nguồn USB, tắt các chân ngoại vi tránh xung đột
#define PIN_VBAT_READ  -1
#define LED            -1
#define ALARM_PIN      -1
#endif
#define LORA_RST_PIN   8
#define LORA_BUSY_PIN  16
#define LORA_DIO1_PIN  20

// --- Địa chỉ Gateway cố định ---
#define GATEWAY_ID     0

// --- Tham số sóng vô tuyến đồng bộ toàn hệ thống ---
#define LORA_FREQUENCY        921.4f  // MHz
#define LORA_BANDWIDTH        125.0f  // kHz
#define LORA_SPREADING_FACTOR 9       // SF9
#define LORA_CODING_RATE      7       // 4/7
#define LORA_TX_POWER         22      // +22 dBm
```

---

## 3. Cấu Hình Môi Trường Firmware (PlatformIO)

Trong file `platformio.ini`, Gateway được cấu hình độc lập trong môi trường `[env:gateway]`:

```ini
[env:gateway]
upload_port = COM12       ; Thay bằng cổng COM của thiết bị Gateway khi cắm vào máy tính
monitor_port = COM12
upload_speed = 921600
build_unflags = ${usb_ttl.build_unflags}
build_flags = 
	${env.build_flags} 
	${usb_ttl.build_flags}
	-D ROLE_GATEWAY

lib_deps =
	${env.lib_deps}
	bblanchon/ArduinoJson @ ^6.21.3

build_src_filter =
	+<main_gateway.cpp>
	+<lora_mesh/>
	-<main_repeater_full.cpp>
	-<main_repeater_simple.cpp>
	-<main_node.cpp>
	-<capture/>
	-<keyword_spot/>
	-<stereo_process/>
	-<tier0/>
	-<ringbuffer/>
	-<ble/>
```

---

## 4. Đặc Tả Giao Thức Dữ Liệu USB Serial NDJSON

Giao tiếp giữa Gateway và Máy tính tuân theo nguyên tắc: **Mỗi thông điệp là một dòng JSON hợp lệ kết thúc bằng ký tự xuống dòng `\n`**.

### 4.1. Lệnh từ Máy Tính gửi xuống Gateway

#### 1. Lệnh gửi tin nhắn Chat tới Node:
```text
CHAT:<client_id>:<target_node>:<utf8_text>\n
```
* `<client_id>`: Mã số tin nhắn do phần mềm quản lý tự sinh (ví dụ: `2001`).
* `<target_node>`: Địa chỉ Node nhận (`1`..`254` = Node cụ thể; `255` = Broadcast toàn mạng).
* `<utf8_text>`: Nội dung tin nhắn UTF-8 có dấu (tối đa 128 bytes).

**Ví dụ:**
```text
CHAT:2001:146:Đội 1 lập tức rút về phía Đông do lũ quét\n
```

#### 2. Lệnh điều khiển bật / tắt còi báo động tại Trạm Lặp (Gồm đúng 4 lệnh chuẩn):

| STT | Cú pháp lệnh | Chức năng thực hiện | Cơ chế truyền nhận |
| :---: | :--- | :--- | :--- |
| **1** | `ALARM:ALL:ON\n` | **Bật còi toàn bộ** các Trạm Lặp Repeater | Broadcast toàn mạng, thời lượng 7s |
| **2** | `ALARM:ALL:OFF\n` | **Tắt còi toàn bộ** các Trạm Lặp Repeater | Broadcast toàn mạng, ngắt còi lập tức |
| **3** | `ALARM:<id>:ON\n` | **Bật còi tại Repeater chỉ định** | Unicast đích danh + Chờ ACK phản hồi |
| **4** | `ALARM:<id>:OFF\n` | **Tắt còi tại Repeater chỉ định** | Unicast đích danh + Chờ ACK phản hồi |

*Ví dụ thực tế:*
* `ALARM:ALL:ON\n`: Kích hoạt còi báo động trên toàn bộ mạng lưới Repeater.
* `ALARM:ALL:OFF\n`: Tắt còi trên toàn bộ mạng lưới ngay lập tức.
* `ALARM:2:ON\n`: Bật còi riêng tại Repeater có ID là `2`.
* `ALARM:2:OFF\n`: Tắt còi tại Repeater ID `2`.
*(Hệ thống vẫn hỗ trợ thêm tham số thời lượng tùy chọn nếu cần, ví dụ: `ALARM:ALL:ON:15` hoặc `ALARM:2:ON:10`).*

### 4.2. Các sự kiện Gateway xuất lên Máy Tính (JSON Events)

#### 1. Sự kiện sẵn sàng khi khởi động (`GATEWAY_READY`):
```json
{"type":"GATEWAY_READY","node_id":0,"mac":"34:85:18:XX:XX:XX"}
```

#### 2. Cảnh báo cứu nạn khẩn cấp từ hiện trường (`SOS`):
```json
{
  "type": "SOS",
  "src": 146,
  "alert": "BAWL",
  "confidence": 94,
  "dir": "L",
  "battery": 4120,
  "msg_id": 42,
  "rssi": -68.5,
  "snr": 9.2
}
```
* `alert`: Loại cảnh báo (`BAWL` = Tiếng kêu khóc/cứu, `TEST` = Thử nghiệm).
* `confidence`: Độ tin cậy nhận diện của AI (%).
* `dir`: Hướng âm thanh (`L`: Trái, `R`: Phải, `C`: Giữa).
* `battery`: Điện áp pin của Node gửi (mV).

#### 3. Tin nhắn Chat nhận được từ hiện trường (`CHAT`):
```json
{
  "type": "CHAT",
  "src": 162,
  "dest": 0,
  "text": "Báo cáo: Đã tìm thấy 02 nạn nhân, cần tiếp tế nước uống",
  "msg_id": 210,
  "hops": 2,
  "rssi": -74.0,
  "snr": 7.8
}
```

#### 4. Cập nhật trạng thái tin nhắn do Gateway phát đi (`CHAT_STATUS`):
* Đang phát sóng:
  ```json
  {"type":"CHAT_STATUS","client_id":2001,"msg_id":315,"status":"SENDING"}
  ```
* Node đích đã nhận tin và trả ACK:
  ```json
  {"type":"CHAT_STATUS","client_id":2001,"msg_id":315,"status":"DELIVERED"}
  ```
* Không có phản hồi sau 3 lần thử (khoảng 6 giây):
  ```json
  {"type":"CHAT_STATUS","client_id":2001,"msg_id":315,"status":"FAILED","reason":"NO_ACK"}
  ```

#### 5. Sự kiện phát lệnh điều khiển còi (`ALARM_CMD_SENT`):
```json
{"type":"ALARM_CMD_SENT","target":2,"action":"ON","duration":15}
```

#### 6. Xác nhận Trạm Lặp thực thi lệnh còi thành công / thất bại (`CMD_STATUS`):
* Khi Repeater đích nhận lệnh và trả gói ACK thành công:
  ```json
  {"type":"CMD_STATUS","cmd":"ALARM","msg_id":105,"src":2,"status":"ACK_CONFIRMED","rtt_ms":195}
  ```
* Khi không nhận được phản hồi sau số lần thử (Timeout):
  ```json
  {"type":"CMD_STATUS","cmd":"ALARM","msg_id":105,"target":2,"status":"FAILED","reason":"NO_ACK"}
  ```
* Khi cú pháp lệnh hoặc tham số không hợp lệ:
  ```json
  {"type":"CMD_STATUS","cmd":"ALARM","status":"FAILED","reason":"BAD_FORMAT"}
  ```

> [!NOTE]
> **Đặc tả 100% Chuẩn NDJSON thuần khiết trên Gateway:**
> - Toàn bộ các dòng log gỡ lỗi dạng văn bản (`[RX]`, `[TX]`, `[CMD]`, `[QoS]`, `[RELAY]`, `[STATUS]`...) và banner khởi động `===` đều đã được lọc bỏ hoàn toàn khi biên dịch cho `ROLE_GATEWAY`.
> - Tính năng báo cáo Heartbeat đã được loại bỏ tại Gateway để giữ cho luồng dữ liệu đẩy lên Dashboard hoàn toàn gọn gàng, chỉ tập trung vào các sự kiện tác chiến quan trọng (SOS, Chat, Điều khiển còi, Xác nhận trạng thái lệnh).
> - Mọi dòng xuất ra cổng Serial của Gateway đều là 1 đối tượng JSON duy nhất kết thúc bằng ký tự `\n`, đảm bảo parser phía Backend/Dashboard luôn parse thành công 100% mà không bị lỗi JSON decode.

---

## 5. Mã Nguồn Mẫu Ứng Dụng Máy Tính (Python Serial Bridge)

Một chương trình Python hoàn chỉnh giúp kỹ sư giao tiếp hai chiều với Gateway:

```python
import serial
import json
import threading
import sys
import time

SERIAL_PORT = 'COM12'  # Thay đổi thành cổng COM thực tế của Gateway
BAUD_RATE = 115200

def reader_thread(ser):
    while True:
        try:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            
            # Kiểm tra nếu là dòng JSON hợp lệ
            if line.startswith('{') and line.endswith('}'):
                try:
                    data = json.loads(line)
                    msg_type = data.get('type')
                    if msg_type == 'SOS':
                        print(f"\n🚨 [CẢNH BÁO SOS] Từ Node #{data.get('src')}: {data.get('alert')} ({data.get('confidence')}%) Hướng:{data.get('dir')} | Pin:{data.get('battery')}mV (RSSI:{data.get('rssi')}dBm)")
                    elif msg_type == 'CHAT':
                        print(f"\n💬 [TIN NHẮN ĐẾN] Từ Node #{data.get('src')} -> Đích #{data.get('dest')} (Hops:{data.get('hops')}): \"{data.get('text')}\"")
                    elif msg_type == 'CHAT_STATUS':
                        print(f"\n📡 [TRẠNG THÁI GỬI] ClientID:{data.get('client_id')} -> {data.get('status')} {data.get('reason', '')}")
                    elif msg_type == 'HEARTBEAT':
                        print(f"\n💓 [HEARTBEAT] Trạm #{data.get('src')}: Pin {data.get('battery')}mV | RSSI:{data.get('rssi')}dBm")
                    else:
                        print(f"\n[EVENT] {data}")
                except json.JSONDecodeError:
                    print(f"[RAW-JSON-ERR] {line}")
            else:
                print(f"[SYS] {line}")
        except Exception as e:
            print(f"[LỖI ĐỌC] {e}")
            break

def main():
    print(f"Đang mở cổng {SERIAL_PORT} @ {BAUD_RATE}...")
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except Exception as e:
        print(f"Không thể mở cổng COM: {e}")
        return

    t = threading.Thread(target=reader_thread, args=(ser,), daemon=True)
    t.start()

    print("==========================================================")
    print(" SẴN SÀNG NHẬN DỮ LIỆU TỪ LORA GATEWAY")
    print(" Cú pháp gửi tin: <target_node_id> <nội dung>")
    print(" Ví dụ: 146 Xin chao doi tuan tra so 1")
    print(" Gõ 'q' để thoát")
    print("==========================================================")

    client_counter = 1
    while True:
        try:
            user_input = input()
            if not user_input:
                continue
            if user_input.strip().lower() == 'q':
                break

            parts = user_input.split(' ', 1)
            if len(parts) < 2:
                print("⚠️ Sai cú pháp! Nhập: <target_node_id> <nội dung>")
                continue

            target_node = parts[0]
            text = parts[1]
            cmd = f"CHAT:{client_counter}:{target_node}:{text}\n"
            ser.write(cmd.encode('utf-8'))
            print(f"➔ Đã gửi lệnh: {cmd.strip()}")
            client_counter += 1
        except KeyboardInterrupt:
            break

    ser.close()
    print("Đã đóng kết nối Gateway.")

if __name__ == '__main__':
    main()
```

---

## 6. 🤖 Prompt Chuẩn Cho AI Để Tích Hợp Vào Dự Án Gateway

Sao chép toàn bộ khối nội dung bên dưới và dán vào trợ lý AI (ChatGPT, Claude, Antigravity, Copilot) để AI tự động tích hợp mã nguồn Gateway vào dự án hoặc ứng dụng máy tính của bạn:

```markdown
Hãy giúp tôi tích hợp chức năng "Trạm Cổng Chỉ Huy Gateway (LoRa Mesh + USB Serial NDJSON)" vào dự án của tôi.

Dưới đây là các yêu cầu và đặc tả kỹ thuật cần tuân thủ:

1. PHẦN CỨNG & VAI TRÒ GATEWAY:
- Vi điều khiển: ESP32 hoặc ESP32-S3.
- Module LoRa: SX1262 (sử dụng thư viện RadioLib).
- Giao tiếp: SPI với các chân CS, MOSI, MISO, SCK, RST, BUSY, DIO1 (hãy hỏi tôi sơ đồ chân GPIO của bo mạch Gateway tôi đang dùng nếu chưa có trong code).
- Tham số RF: Tần số 921.4 MHz, SF 9, BW 125.0 kHz, CR 4/7, TX Power +22 dBm, Preamble 16 symbols.
- Định danh Gateway: Node ID cố định là 0 (GATEWAY_ID = 0).

2. GIAO DIỆN KẾT NỐI MÁY TÍNH (PC USB SERIAL):
- Tốc độ baud: 115200.
- Định dạng dữ liệu: Chuẩn NDJSON (Newline Delimited JSON) - mỗi bản tin là 1 chuỗi JSON trên 1 dòng đơn kết thúc bằng ký tự '\n'.
- Không sử dụng các phím tắt console ký tự đơn lẻ.

3. ĐỊNH DẠNG CÁC BẢN TIN JSON XUẤT LÊN MÁY TÍNH:
- Khi khởi động sẵn sàng:
  {"type":"GATEWAY_READY","node_id":0,"mac":"<mac_address>"}
- Cảnh báo cứu nạn khẩn cấp từ hiện trường:
  {"type":"SOS","src":<node_id>,"alert":"<BAWL|TEST>","confidence":<0-100>,"dir":"<L|R|C>","battery":<mv>,"msg_id":<num>,"rssi":<val>,"snr":<val>}
- Tin nhắn Chat từ hiện trường chuyển về:
  {"type":"CHAT","src":<node_id>,"dest":0,"text":"<nội dung utf8>","msg_id":<num>,"hops":<num>,"rssi":<val>,"snr":<val>}
- Cập nhật trạng thái tin nhắn do Gateway phát đi:
  {"type":"CHAT_STATUS","client_id":<id>,"msg_id":<num>,"status":"<SENDING|DELIVERED|FAILED>","reason":"<lý do nếu có>"}
- Báo cáo định kỳ sức khỏe trạm lặp (Repeater):
  {"type":"HEARTBEAT","src":<node_id>,"battery":<mv>,"role":"REPEATER","rssi":<val>,"snr":<val>}

4. LỆNH ĐIỀU KHIỂN TỪ MÁY TÍNH GỬI XUỐNG GATEWAY:
- Cú pháp gửi tin nhắn ra mạng lưới:
  CHAT:<client_id>:<target_node>:<utf8_text>\n
  (target_node là ID của Node/Balo hoặc 255 để gửi Broadcast).
- Gateway tự động quản lý gửi lại Stop-and-Wait ARQ (timeout 2000ms, retry 3 lần) và báo trạng thái CHAT_STATUS về máy tính.

5. YÊU CẦU TÍCH HỢP:
- Viết mã nguồn non-blocking trong loop(): gọi lora_mesh_loop(), phân tích cú pháp chuỗi Serial đệm và xuất JSON chuẩn xác.
- Hướng dẫn tôi cấu hình platformio.ini và cách kết nối với ứng dụng máy tính (Python/NodeJS/Web Serial).
```
