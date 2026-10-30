# 🌲 Hệ Thống Trạm Lặp Cứu Hộ Rừng Thông Minh (ESP32-S3 LoRa Mesh & KWS AI)

Hệ thống trạm lặp (Repeater) cứu hộ khẩn cấp thế hệ mới, tích hợp **Trí tuệ nhân tạo biên (Edge AI)** để nhận diện tiếng kêu cứu và **Mạng truyền thông diện rộng LoRa Mesh tiết kiệm năng lượng kép** (chuẩn MeshCore PowerSaving).

Hệ thống được thiết kế đặc thù cho môi trường rừng núi hiểm trở: hoạt động độc lập 24/7 bằng nguồn pin/năng lượng mặt trời, phát hiện người gặp nạn theo thời gian thực và tự động chuyển tiếp tín hiệu cảnh báo về trung tâm chỉ huy qua mạng lưới trạm lặp đa chặng (Multi-hop Mesh).

---

## 📌 Các Tính Năng Cốt Lõi

1. **Nhận diện tiếng kêu cứu tại biên (On-device KWS AI)**:
   - Mô hình mạng nơ-ron tích chập (CNN int8) chạy trực tiếp trên vi điều khiển ESP32-S3, độ trễ thấp, không phụ thuộc internet hay sóng di động.
   - Kiến trúc lọc 2 tầng (Cascade Pipeline): **VAD (Voice Activity Detection)** lọc năng lượng âm thanh thô $\rightarrow$ **KWS (Keyword Spotting)** suy luận từ khóa.
   - Cơ chế tích lũy độ tin cậy đa khung (Confidence Accumulation) loại bỏ triệt để báo động giả do chim hót, gió bão hoặc suối chảy.
2. **Hệ thống Mic kép (Dual MEMS) & Định vị hướng âm thanh**:
   - Sử dụng 2 micro kỹ thuật số ICS-43434 (Stereo I2S).
   - Tự động so sánh năng lượng và tương quan kênh Trái / Phải để ước lượng hướng người bị nạn (**Trái / Phải / Chính giữa**).
   - Tự động phát hiện lỗi phần cứng (bụi bẩn, nước, che mic) và chuyển sang chế độ đơn kênh dự phòng (Single-mic fallback).
3. **Mạng truyền thông LoRa Mesh Ad-hoc tự tổ chức**:
   - Hoạt động ở dải tần **921.4 MHz** (hợp quy chuẩn AS923-2 tại Việt Nam), công suất phát tối đa +22 dBm với module Semtech **SX1262 (Ai-Thinker Ra-01SH)**.
   - **Dynamic Node ID**: Tự động gán ID duy nhất từ byte cuối của địa chỉ MAC phần cứng (eFuse), cắm là chạy, không cần cấu hình mã tĩnh.
   - **Định tuyến Unicast Next-Hop**: Tự động học đường truyền ngược (Reverse Path Learning) và gửi đích danh qua trạm kế cận (Next-Hop Unicast), chỉ chuyển sang Broadcast Flood khi khám phá tuyến đường mới.
   - **Lọc gói tin thông minh (Next-Hop Filtering)**: Các node trung gian không nằm trong lộ trình sẽ tự động bỏ qua gói (`[SKIP]`), giải phóng hoàn toàn băng thông mạng RF.
   - **Chống lặp & chống vòng tròn**: Bộ đệm Deduplication nhớ 32 gói gần nhất, giới hạn 7 bước nhảy (Hop Limit) và lưu vết hành trình gói tin (`path[8]`).
   - **Độ tin cậy QoS (Stop-and-Wait ARQ)**: Tự động gửi gói tin xác nhận ACK, hỗ trợ phát lại (Retries tối đa 3 lần) khi rớt gói.
   - **Tránh va chạm sóng (CAD + CSMA)**: Quét kênh trước khi phát với cơ chế ngẫu nhiên hóa thời gian chờ (Slot Backoff) theo Node ID.
4. **Tiết kiệm năng lượng kép (Dual Power Saving - MeshCore Style)**:
   - **Tầng 1 (RF - SX1262)**: Chế độ thu chu kỳ động (**Dynamic RXPS Balanced Level 6** kết hợp Preamble 16 symbols). Radio tự động thức ~49.5ms / ngủ ~23.7ms, giảm ~60% năng lượng thu chờ mà vẫn bắt 100% gói tin.
   - **Tầng 2 (MCU - ESP32-S3)**: Chế độ **Light Sleep** cho trạm lặp (Repeater), đưa dòng tiêu thụ MCU xuống còn ~1.5 - 2mA. Thức dậy tức thì (<0.5ms) khi chân ngắt `DIO1` báo có gói tin tới.
   - **Bảo vệ pin kiệt (Brownout Protection)**: Giám sát điện áp pin định kỳ qua cầu phân áp ADC; tự động vào Deep Sleep ngắt tải nếu pin xuống dưới 3.0V.
5. **Watchdog RX (RXPS Watchdog) — Khả năng tự phục hồi 100%**:
   - Giám sát kẹt chân phần cứng `BUSY` (>50ms).
   - Giám sát mất giao tiếp SPI hoặc thanh ghi IRQ bị treo (`0xFFFF`).
   - Tự động làm tươi (resync) chu kỳ RXPS sau 45 giây im lặng, đảm bảo hệ thống không bao giờ bị "chết lâm sàng".
6. **Gói tin SOS nhị phân siêu gọn (Binary Payload)**:
   - Kích thước payload chỉ **5 bytes**: Loại cảnh báo (1B) + Độ tin cậy (1B) + Hướng (1B) + Điện áp pin (2B).
   - Tổng kích thước toàn bộ gói tin cả Header chỉ **24 bytes**, thời gian truyền sóng (Airtime) cực ngắn (~45ms @ SF9 BW125), tối thiểu hóa nguy cơ va chạm và tiết kiệm pin tối đa.

---

## 🛠️ Cấu Hình Phần Cứng & Kết Nối Chân (Pinout)

Hệ thống xây dựng trên module vi điều khiển **ESP32-S3-DevKitC-1 (8MB Flash, QIO/OPI PSRAM)**:

| Chức năng | Thiết bị ngoại vi | Chân ESP32-S3 (GPIO) | Ghi chú |
| :--- | :--- | :--- | :--- |
| **I2S Mic Kép** | ICS-43434 (L & R) | `GPIO12` (BCK), `GPIO14` (WS/LRCL), `GPIO13` (SD/DATA) | Chung bus 2 micro; Mic L nối GND, Mic R nối VDD |
| **LoRa SPI** | SX1262 (Ra-01SH) | `GPIO7` (SCK), `GPIO6` (MISO), `GPIO5` (MOSI), `GPIO4` (NSS/CS) | Sử dụng HSPI riêng biệt |
| **LoRa Control** | SX1262 Control | `GPIO8` (RST), `GPIO16` (BUSY), `GPIO20` (DIO1) | DIO1 hỗ trợ đánh thức RTC EXT1 từ Light Sleep |
| **Thẻ MicroSD** | SD Card Slot | `GPIO40` (SCK), `GPIO41` (MISO), `GPIO39` (MOSI), `GPIO38` (CS) | Giao tiếp SPI riêng biệt |
| **Cảnh báo tại chỗ** | Còi báo / Relay | `GPIO9` (ALARM_PIN), `GPIO48` (LED RGB/Status) | Kéo còi 7 giây khi có báo động SOS |
| **Giám sát Pin** | Cầu phân áp | `GPIO1` (ADC1 Channel 0) | Cầu trở 100kΩ : 100kΩ (Hệ số x2.0) |

---

## 🏗️ Kiến Trúc Hệ Thống

```
                                  KHOẢNH KHẮC THU ÂM
                                           │
                        ┌──────────────────┴──────────────────┐
                        ▼                                     ▼
               ICS-43434 (Mic Trái)                  ICS-43434 (Mic Phải)
                        └──────────────────┬──────────────────┘
                                           │ (I2S Stereo 16kHz)
                                           ▼
                                [ Bộ đệm vòng RingBuffer ] ◄── Giữ 2 giây Pre-roll
                                           │
                                           ▼
                                  【 TẦNG 0: VAD Gate 】
                                   (Tính năng lượng RMS)
                                           │
                         ┌─────────────────┴─────────────────┐
                    (Âm thanh nhỏ)                      (Năng lượng cao)
                         ▼                                   ▼
                   Cổng đóng (0% AI)               Mở cổng ──► Đánh thức Task AI (Core 1)
                                                                     │
                                                                     ▼
                                                          【 TẦNG 1: Edge AI KWS 】
                                                           - Trích xuất đặc trưng MFCC
                                                           - Model CNN int8 phân loại
                                                           - Tích lũy điểm tin cậy
                                                                     │
                                                      ┌──────────────┴──────────────┐
                                                  (Dưới ngưỡng)                (Xác nhận tiếng cứu)
                                                      ▼                             ▼
                                                   Hủy bỏ                  - Ghi file WAV & CSV lên thẻ SD
                                                                           - Định vị hướng: Trái / Phải / Giữa
                                                                           - Bật còi báo động tại chỗ (GPIO9)
                                                                           - Đóng gói Binary SOS (5 bytes)
                                                                                    │
                                                                                    ▼
                                                                        【 MẠNG LORA MESH SX1262 】
                                                                        - Tự động gán Node ID từ eFuse MAC
                                                                        - Kiểm tra kênh sạch (CAD)
                                                                        - Định tuyến Unicast Next-Hop
                                                                        - Quản lý xác nhận QoS ACK
                                                                        - Tiết kiệm năng lượng kép (RXPS + MCU Sleep)
```

---

## 📡 Hai Môi Trường Firmware (PlatformIO Environments)

Dự án cung cấp 2 chế độ biên dịch tối ưu hóa tùy mục đích sử dụng trong file [platformio.ini](file:///d:/LearnEsp32/Repeater/platformio.ini):

### 1. `node_full` (Trạm Đầu Cuối / Trạm Giám Sát Toàn Diện)
* **Chức năng**: Thu âm I2S 2 micro, chạy VAD, chạy mô hình AI nhận diện tiếng kêu cứu, lưu file WAV lên thẻ nhớ SD và gửi bản tin khẩn cấp SOS về trung tâm.
* **Lệnh nạp**:
  ```bash
  pio run -e node_full --target upload
  ```

### 2. `node_repeater` (Trạm Lặp Trung Chuyển Tối Giản)
* **Chức năng**: Không sử dụng I2S/Audio/SD card. Đóng vai trò là trạm lặp trung gian đặt trên đỉnh núi hoặc cây cao để tiếp sóng LoRa Mesh về Gateway.
* **Đặc tính**: Kích hoạt toàn diện **Tiết kiệm năng lượng kép** (SX1262 RX Duty Cycle + MCU Light Sleep), chạy hàng tháng trời chỉ với một viên pin 18650 và tấm pin năng lượng mặt trời mini.
* **Lệnh nạp**:
  ```bash
  pio run -e node_repeater --target upload
  ```

---

## 💻 Tập Lệnh Điều Khiển Qua Serial Monitor (CLI)

Mở Serial Monitor ở tốc độ baud **115200**:

```bash
pio device monitor -b 115200
```

Các phím tắt điều khiển trực tiếp trên bàn phím:

| Phím tắt | Phạm vi áp dụng | Ý nghĩa & Hành vi |
| :---: | :---: | :--- |
| **`t`** / **`T`** | Cả 2 Node | Gửi gói tin giả lập **TEST SOS** nhị phân về Gateway (Dest: 0), kích hoạt còi và yêu cầu ACK. |
| **`p`** / **`P`** | Cả 2 Node | Gửi **PING BROADCAST** tới toàn bộ mạng lưới để đo đạc chất lượng kết nối và yêu cầu ACK từ mọi node. |
| **`h`** / **`H`** | Cả 2 Node | Gửi bản tin **Heartbeat Telemetry** (báo cáo điện áp pin, trạng thái hoạt động). |
| **`r`** / **`R`** | `node_repeater` | Hiển thị toàn bộ bảng định tuyến **Routing Table** (Địa chỉ đích, Next-Hop, số chặng Hops, RSSI, tuổi tuyến đường). |
| **`m`** / **`M`** | `node_full` | Hiển thị toàn bộ bảng định tuyến **Routing Table** *(thay cho phím 'r' để tránh trùng lệnh thu âm)*. |
| **`w`** / **`W`** | Cả 2 Node | Kiểm tra trạng thái **RX Watchdog** và số lần hệ thống đã tự động khôi phục chip radio SX1262. |
| **`s`** / **`S`** | Cả 2 Node | Kiểm tra trạng thái **Chế độ tiết kiệm năng lượng kép (Dual Power Saving)**. |
| **`r`** / **`R`** | `node_full` | Ghi âm thủ công 1 file WAV 3 giây vào thẻ SD để phục vụ thu thập dữ liệu huấn luyện AI. |
| **`c`** / **`C`** | `node_full` | Bật / tắt chế độ ghi âm WAV liên tục. |
| **`q`** / **`Q`** | `node_full` | Thoát chế độ ghi âm liên tục, khôi phục lại luồng phát hiện AI. |

---

## 📊 Định Dạng Log Hệ Thống Chuẩn Hóa

Hệ thống sử dụng cấu trúc log một dòng với tiền tố nhận diện trực quan:

```text
[STATUS] Up 00:05:30 | Batt:4.12V | RX:12 Relay:8 TX:1 Dup:2 | Routes:3 | WD:0 | Heap:210KB
[RX] SOS #42 | Src:23->Dest:0 (NextHop:23, via:23, hop:0) | -65dBm/9.5dB | BAWL 94% [L] | Batt:4.12V | Path:23
[RELAY] SOS #42 (Unicast -> NextHop:0) -> Hop 1 OK (Path:23->146)
[SKIP] DATA #15: Unicast targeted to NextHop 55 (self is 146)
[QoS] ✓ ACK CONFIRMED for msg #42 from Node 0 (RTT: 92ms)
[WD] Radio unresponsive (IRQ=0xFFFF)! Hard reset (Resets: 1)
[VAD] Gate ON (energy:1850 >= thr:450, floor:150)
[KWS] Match candidate: score=0.91, margin=0.82 (bawl:0.91, noise:0.09)
[SD] Saved /recordings/detected_0005.wav (bawl 91%)
```

---

## 📁 Cấu Trúc Thư Mục Dự Án

```
ESP32-S3-LoRa-Repeater-KWS/
├── include/
│   └── config.h                  # Cấu hình toàn bộ tham số phần cứng, LoRa, AI, VAD, Pin
├── lib/
│   └── KWS/                      # Thư viện mạng nơ-ron Edge Impulse (mô hình int8)
├── src/
│   ├── main_full.cpp             # Điểm chạy chính cho node đầy đủ (AI + Audio + LoRa)
│   ├── main_repeater.cpp         # Điểm chạy chính cho node lặp trung chuyển (LoRa Relay)
│   ├── capture/                  # Tác vụ I2S DMA đọc âm thanh đa kênh real-time
│   ├── tier0/                    # Tầng lọc năng lượng âm thanh VAD (Voice Activity Detection)
│   ├── stereo_process/           # Xử lý âm thanh Stereo kép, cân bằng năng lượng & ước lượng hướng
│   ├── keyword_spot/             # Tầng suy luận AI KWS, trích xuất MFCC & xác nhận từ khóa
│   ├── ringbuffer/               # Bộ đệm vòng PSRAM lưu trữ âm thanh pre-roll
│   └── lora_mesh/                # Driver SX1262, định tuyến Mesh, QoS, Watchdog & RXPS
├── platformio.ini                # Cấu hình nạp firmware, phân luồng build_src_filter và cổng COM
└── README.md                     # Tài liệu hướng dẫn kỹ thuật chi tiết
```

---

## 🚀 Hướng Dẫn Biên Dịch & Nạp Nhanh

1. Cài đặt [PlatformIO IDE](https://platformio.org/) (trên VS Code hoặc CLI).
2. Kết nối board ESP32-S3 vào máy tính.
3. Kiểm tra cổng COM trong [platformio.ini](file:///d:/LearnEsp32/Repeater/platformio.ini) (ví dụ: `upload_port = COM6` cho Repeater, `COM17` cho Node Full).
4. Thực hiện biên dịch và nạp firmware:
   ```bash
   # Dành cho trạm lặp Repeater
   pio run -e node_repeater -t upload

   # Dành cho trạm AI Full
   pio run -e node_full -t upload
   ```
5. Mở Serial Monitor để theo dõi hoạt động:
   ```bash
   pio device monitor -b 115200
   ```
