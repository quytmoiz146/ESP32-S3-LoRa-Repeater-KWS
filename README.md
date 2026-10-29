# 🌲 Mô-đun Nhận Diện Tiếng Kêu Cứu — Trạm Lặp Repeater Cứu Hộ Rừng

## Bối cảnh dự án

Đây là **mô-đun mở rộng** cho trạm lặp (repeater) cứu hộ cứu nạn được lắp đặt trong rừng. Trạm lặp có nhiệm vụ chính là trung chuyển tín hiệu liên lạc giữa các đội cứu hộ và trung tâm chỉ huy. Mô-đun này bổ sung thêm khả năng:

> **Lắng nghe âm thanh môi trường xung quanh 24/7, sử dụng AI trên vi điều khiển để tự động nhận diện tiếng kêu cứu, từ đó phát tín hiệu cảnh báo về trung tâm cứu hộ.**

Kịch bản sử dụng: Một người đi rừng bị lạc hoặc gặp nạn, kêu cứu trong phạm vi thu âm của trạm lặp. Mô-đun AI tự động nhận diện tiếng kêu cứu và kích hoạt quy trình cảnh báo — ngay cả khi không có ai trực tiếp theo dõi.

---

## Phần cứng

Mô-đun gồm 3 thành phần chính:

- **ESP32-S3** — Vi điều khiển lõi kép, có PSRAM, đủ mạnh để chạy mô hình AI nhẹ ngay trên chip mà không cần kết nối internet.
- **ICS-43434** — Microphone MEMS kỹ thuật số giao tiếp I2S, cho chất lượng thu âm ổn định trong điều kiện ngoài trời.
- **Thẻ SD** — Lưu trữ file audio khi phát hiện tiếng kêu cứu (để xác minh lại) và log sự kiện.

---

## Luồng hoạt động chính

Toàn bộ hệ thống hoạt động theo kiến trúc **2 tầng cascade**, trong đó tầng rẻ lọc trước cho tầng đắt — giúp tiết kiệm năng lượng vì AI chỉ chạy khi thực sự cần thiết.

### Tổng quan

```
Mic thu âm liên tục ──► Lọc năng lượng (VAD) ──► Nhận diện từ khóa (AI) ──► Phát cảnh báo cứu hộ
        │                     │                          │
   luôn hoạt động        rất nhẹ, ~32ms            chỉ chạy khi
   tiêu thụ ít           mỗi lần xử lý          VAD phát hiện có
                                                 tiếng người nói
```

### Chi tiết từng bước

**① Thu âm liên tục**

Microphone ICS-43434 thu âm 16.000 mẫu/giây qua giao tiếp I2S. Dữ liệu audio được đọc liên tục theo từng khối nhỏ (~32ms/khối) và lưu vào một **bộ đệm vòng (ring buffer)** trên PSRAM. Bộ đệm này giữ lại 2 giây audio gần nhất — nhờ vậy khi AI bắt đầu phân tích, nó có cả phần âm thanh *trước* thời điểm phát hiện, không bỏ lỡ phần đầu câu kêu cứu.

**② Tầng lọc — VAD (Voice Activity Detection)**

Mỗi khối audio vừa thu, hệ thống tính **năng lượng RMS** (mức to trung bình) và so sánh với mức nền (noise floor) của môi trường. Nếu năng lượng vượt gấp nhiều lần mức nền — nghĩa là có ai đó đang nói/hét/kêu — tầng VAD sẽ "mở cổng" để đánh thức tầng AI.

VAD hoạt động theo máy trạng thái 3 pha:

```
 Khởi động → [Đo mức nền] → [Chờ tín hiệu] ↔ [Có tín hiệu — mở cổng]
              2 giây đầu      yên tĩnh,         có tiếng → đánh thức AI
                               cập nhật          giữ cổng mở qua khoảng
                               mức nền           lặng ngắn giữa các từ
```

- **Đo mức nền (2 giây đầu):** Sau khi bật nguồn, hệ thống lắng nghe 2 giây đầu tiên để đo mức tiếng ồn tự nhiên của môi trường (tiếng gió, tiếng côn trùng...) làm chuẩn so sánh.
- **Chờ tín hiệu:** So sánh năng lượng hiện tại với mức nền. Âm thanh bình thường → bỏ qua, đồng thời liên tục cập nhật mức nền để thích nghi khi môi trường thay đổi (gió to hơn, trời mưa...).
- **Có tín hiệu:** Khi năng lượng vượt ngưỡng → mở cổng, đánh thức tầng AI. Cổng được giữ mở một khoảng ngắn qua các khoảng lặng giữa các từ trong câu, tránh bị đóng giữa chừng khi người ta đang nói "cứu... tôi...". Có cơ chế timeout 30 giây để tự đóng nếu âm thanh kéo dài bất thường (ví dụ tiếng suối chảy liên tục bị nhầm là tín hiệu).

**Mục đích của tầng VAD:** Đảm bảo AI không chạy 24/7 — chỉ khi thực sự có âm thanh đáng chú ý. Điều này rất quan trọng cho trạm lặp trong rừng vì thiết bị thường chạy bằng pin hoặc năng lượng mặt trời.

**③ Tầng nhận diện — KWS (Keyword Spotting)**

Khi được VAD đánh thức, tầng AI thực hiện:

1. **Đọc 1 giây audio** gần nhất từ bộ đệm vòng (bao gồm cả phần âm thanh trước thời điểm VAD kích hoạt).
2. **Trích xuất đặc trưng MFCC** — biến đổi audio thô thành "vân tay âm thanh" mô tả đặc điểm tần số giọng nói theo thời gian.
3. **Chạy mô hình CNN quantized int8** (Edge Impulse, biên dịch bằng EON Compiler) — phân loại đoạn audio thành 2 lớp: `bawl` (tiếng kêu cứu) hoặc `noise` (tiếng ồn/không liên quan).
4. **Tích lũy độ tin cậy qua nhiều lần inference** — không quyết định dựa trên 1 lần duy nhất mà yêu cầu nhiều frame liên tiếp xác nhận cùng 1 kết quả. Cơ chế này hoạt động giống cách con người nghe: phải nghe được vài âm tiết liên tục mới tin là có người đang kêu cứu, tránh báo động giả do tiếng chim, tiếng gió thoảng.
5. **Xác nhận phát hiện** — khi tổng điểm tích lũy vượt ngưỡng:
   - Lưu đoạn audio xung quanh thời điểm phát hiện thành file WAV trên thẻ SD (để đội cứu hộ có thể nghe lại xác minh).
   - Ghi log sự kiện vào file CSV (thời điểm, độ tin cậy, thời gian xử lý).
   - Kích hoạt tín hiệu cảnh báo cứu hộ (gửi qua hệ thống repeater).
   - Tạm nghỉ 3 giây trước khi phân tích tiếp (tránh kích hoạt cảnh báo liên tục cho cùng 1 sự kiện).

**④ Phát cảnh báo**

Khi AI xác nhận có tiếng kêu cứu, thông tin detection (nhãn, độ tin cậy, timestamp) được gửi đi qua hệ thống truyền thông của trạm lặp về trung tâm cứu hộ.

---

## Cơ chế tích lũy độ tin cậy

Đây là điểm khác biệt quan trọng so với cách tiếp cận "1 lần nhận diện → kích hoạt". Hệ thống yêu cầu nhiều frame AI liên tiếp xác nhận cùng 1 keyword:

- Mỗi frame AI trả về kết quả `bawl` → **cộng** điểm tin cậy vào bộ đếm tích lũy.
- Mỗi frame không phải keyword → **trừ** một lượng nhỏ (nhưng không reset về 0 — cho phép 1-2 frame ngắt quãng giữa chừng mà không mất hết điểm).
- Khi tổng tích lũy vượt ngưỡng → **xác nhận phát hiện**.

Ví dụ minh họa:

```
Frame 1: "bawl" 70%  → tích lũy = 0.70
Frame 2: "bawl" 65%  → tích lũy = 1.35  ✅ Xác nhận! (vượt ngưỡng 1.2)
```

```
Frame 1: "bawl" 55%  → tích lũy = 0.55
Frame 2: "noise" 90% → tích lũy = 0.25  (trừ penalty)
Frame 3: "bawl" 80%  → tích lũy = 1.05  (chưa đủ)
Frame 4: "bawl" 60%  → tích lũy = 1.65  ✅ Xác nhận!
```

Cơ chế này giúp giảm đáng kể báo động giả trong môi trường rừng — nơi có rất nhiều âm thanh tự nhiên (chim hót, gió, mưa, suối) có thể bị nhầm lẫn.

---

## Phân chia xử lý song song (FreeRTOS)

ESP32-S3 có 2 lõi CPU, hệ thống phân chia công việc:

- **Core 0 — Thu âm real-time:** Chuyên đọc dữ liệu từ microphone và chạy VAD. Được ưu tiên cao nhất vì nếu bị trễ sẽ mất mẫu audio.
- **Core 1 — Xử lý AI:** Chạy inference model KWS. Tính toán nặng nhưng không cần real-time — nếu bị chậm 1-2 frame cũng không ảnh hưởng kết quả.

Khi không có tín hiệu đáng chú ý (phần lớn thời gian), tầng AI nằm ở trạng thái **ngủ sâu** — tốn 0% CPU, chỉ thức dậy khi VAD gửi notification.

---

## Chế độ ghi âm thủ công

Ngoài chế độ detection tự động, hệ thống hỗ trợ ghi âm WAV thủ công qua Serial để thu thập dữ liệu huấn luyện AI:

| Lệnh | Chức năng |
|---|---|
| `r` | Ghi 1 file WAV 3 giây (tạm dừng detection, ghi xong bật lại) |
| `c` | Bật/tắt ghi liên tục |
| `q` | Dừng ghi, bật lại detection |

Detection và ghi âm không chạy đồng thời (cùng dùng 1 cổng I2S), nên hệ thống có cơ chế tạm dừng — bật lại pipeline an toàn, tránh xung đột.

---

## Cấu trúc mã nguồn

```
src/
├── main.cpp              Điều phối hệ thống, ghi WAV thủ công
├── capture/              Thu âm I2S, ghi ring buffer, chạy VAD
├── tier0/                Máy trạng thái VAD (lọc năng lượng)
├── keyword_spot/         Inference AI + tích lũy confidence
└── ringbuffer/           Bộ đệm vòng PSRAM (giữ pre-roll audio)

include/
└── config.h              Tất cả tham số cấu hình

lib/
└── ei-nhan-dien-cuu/     Thư viện Edge Impulse (model AI int8)
```

---

## Mô hình AI

- **Nguồn:** Edge Impulse — project `kws1`
- **Phân loại:** 2 lớp — `bawl` (kêu cứu) và `noise` (tiếng ồn)
- **Input:** 1 giây audio (16kHz, mono)
- **DSP:** MFCC (trích xuất đặc trưng tần số giọng nói)
- **Model:** CNN quantized int8 + EON Compiler — tối ưu cho vi điều khiển, chạy trực tiếp trên ESP32-S3 mà không cần kết nối cloud.

---

## Build & Flash

```bash
pio run -e esp32-s3-devkitc-1                    # Build
pio run -e esp32-s3-devkitc-1 --target upload    # Upload
pio device monitor -b 115200                      # Serial monitor
```
