# Cơ chế xác nhận phát hiện Tier 1 — Nâng cấp từ cộng dồn điểm đơn thuần

## 1. Vấn đề với cơ chế hiện tại

Cơ chế cũ: cộng dồn confidence qua các frame liên tiếp, trigger khi tổng vượt ngưỡng cố định (~1.2).

```cpp
score += confidence;
if (score > 1.2) trigger();
```

### Thất bại theo 2 hướng trong môi trường nhiều âm thanh hỗn tạp

| Hướng lỗi | Nguyên nhân |
|---|---|
| **Bỏ sót (miss)** | Trong môi trường ồn, confidence của bawl ở mỗi frame bị pha loãng (model không chắc chắn tuyệt đối) → mỗi frame đóng góp điểm nhỏ → không đủ tích lũy đến ngưỡng trước khi refractory/timeout reset |
| **Báo giả (false positive)** | Nhiều loại âm thanh khác nhau khiến rải rác các frame không liên quan đều cho confidence bawl vừa phải → cộng dồn không có decay khiến các điểm rải rác (không đến từ 1 sự kiện liên tục) vẫn tích lũy lên ngưỡng |

**Gốc rễ**: cộng dồn tuyệt đối không phân biệt được *"1 sự kiện thật kéo dài"* với *"nhiều đốm nhiễu rải rác theo thời gian"*, và không có yếu tố so sánh tương đối với các class khác (noise, speech).

---

## 2. Cơ chế mới — 3 kỹ thuật kết hợp

### 2.1. Margin so với class cạnh tranh (thay ngưỡng tuyệt đối)

Thay vì chỉ nhìn confidence tuyệt đối của bawl, tính **khoảng cách (margin)** giữa bawl và class mạnh nhất còn lại:

```cpp
float margin = bawl_confidence - max(noise_confidence, speech_confidence);
```

**Lý do**: trong môi trường ồn, cả 3 class có thể đều nhận confidence vừa phải (model phân vân). Ngưỡng tuyệt đối không phân biệt được "bawl thắng áp đảo" với "bawl chỉ nhỉnh hơn chút". Margin đảm bảo chỉ tích lũy khi model thực sự tự tin bawl vượt trội, không phải khi mọi class đều mờ nhạt.

### 2.2. Moving average trên cửa sổ trượt (thay cộng dồn vô hạn)

```cpp
#define WINDOW_N 10  // số frame gần nhất, tinh chỉnh theo tốc độ inference thực tế

float recent_margins[WINDOW_N] = {0};
int write_idx = 0;

void updateScore(float margin) {
    recent_margins[write_idx] = margin;
    write_idx = (write_idx + 1) % WINDOW_N;
}

float getMovingAverage() {
    float sum = 0;
    for (int i = 0; i < WINDOW_N; i++) sum += recent_margins[i];
    return sum / WINDOW_N;
}
```

**Lý do**: điểm cũ tự động "rơi ra" khỏi cửa sổ thay vì tồn tại mãi. Noise rải rác không đủ liên tục để kéo trung bình lên ngưỡng, nhưng sự kiện bawl thật kéo dài (nhiều frame liên tiếp margin cao) vẫn kéo trung bình lên nhanh.

### 2.3. Ngưỡng kép (hysteresis) + thời lượng tối thiểu liên tục

State machine 3 trạng thái thay vì so sánh 1 ngưỡng:

```
IDLE ──(avg_margin > THRESHOLD_LOW, K frame liên tiếp)──> CANDIDATE
CANDIDATE ──(duy trì đủ MIN_DURATION_MS)──> CONFIRMED ──> trigger()
CANDIDATE ──(avg_margin rớt dưới THRESHOLD_LOW trước khi đủ duration)──> IDLE
```

```cpp
enum DetectState { IDLE, CANDIDATE, CONFIRMED };

DetectState state = IDLE;
uint32_t candidate_start_ms = 0;

#define THRESHOLD_LOW   0.15f   // ngưỡng bắt đầu theo dõi — tinh chỉnh theo dữ liệu thực
#define MIN_DURATION_MS 500     // phải duy trì liên tục tối thiểu 500ms mới xác nhận

void updateStateMachine(float avg_margin, uint32_t now_ms) {
    switch (state) {
        case IDLE:
            if (avg_margin > THRESHOLD_LOW) {
                state = CANDIDATE;
                candidate_start_ms = now_ms;
            }
            break;

        case CANDIDATE:
            if (avg_margin <= THRESHOLD_LOW) {
                state = IDLE;  // rớt ngưỡng trước khi đủ thời lượng -> reset, không trigger
            } else if (now_ms - candidate_start_ms >= MIN_DURATION_MS) {
                state = CONFIRMED;
            }
            break;

        case CONFIRMED:
            triggerAlert();
            state = IDLE;  // hoặc chuyển sang refractory period 3s như thiết kế hiện tại
            break;
    }
}
```

**Lý do**: yêu cầu tính liên tục theo thời gian (loại bỏ noise rải rác ngẫu nhiên không liên tục) mà không cần từng frame phải cực kỳ chắc chắn (giải quyết vấn đề confidence bị pha loãng — chỉ cần vượt ngưỡng thấp hơn nhưng đều đặn).

---

## 3. Nâng cao (tùy chọn) — Ngưỡng thích ứng theo mức nhiễu môi trường

Tận dụng adaptive noise floor đã có sẵn ở Tier 0 (VAD năng lượng), chia sẻ thông tin sang Tier 1 thay vì để 2 tầng hoạt động độc lập:

```cpp
float adaptive_threshold_low = THRESHOLD_LOW + K_ADAPT * (current_noise_floor - baseline_noise_floor);
// K_ADAPT: hệ số tỷ lệ, tinh chỉnh thực nghiệm (đề xuất bắt đầu ~0.01-0.02)
```

**Lý do**: hệ thống tự siết chặt tiêu chí khi môi trường phức tạp (nhiều tạp âm), và nới lỏng khi môi trường yên tĩnh (dễ bắt tín hiệu yếu hơn) — thay vì dùng 1 ngưỡng cứng cho mọi điều kiện.

---

## 4. Thứ tự triển khai đề xuất

Không cần làm cả 4 kỹ thuật cùng lúc — triển khai tăng dần, đo lại hiệu quả sau mỗi bước:

1. **Margin thay ngưỡng tuyệt đối** — sửa đơn giản nhất, tác động lớn nhất tới vấn đề báo giả/bỏ sót hiện tại
2. **Moving average thay cộng dồn vô hạn** — giải quyết vấn đề noise rải rác kéo điểm lên dần
3. **Hysteresis 2 trạng thái + min duration** — nếu bước 1-2 chưa đủ, thêm yêu cầu tính liên tục
4. **Ngưỡng thích ứng theo noise floor** — tinh chỉnh nâng cao, làm sau khi 3 bước trên đã ổn định trên thực địa

## 5. Tham số cần tinh chỉnh thực nghiệm

| Tham số | Giá trị khởi điểm | Ghi chú |
|---|---|---|
| `WINDOW_N` | 10 frame | Phụ thuộc tốc độ inference thực tế trên ESP32-S3 — số frame tương ứng khoảng 1-2s dữ liệu |
| `THRESHOLD_LOW` | 0.15 | Ngưỡng margin bắt đầu theo dõi candidate — thấp hơn nhiều so với ngưỡng trigger cũ (1.2) vì giờ đòi hỏi thêm điều kiện thời lượng |
| `MIN_DURATION_MS` | 500ms | Phải ngắn hơn độ dài trung bình 1 tiếng kêu cứu thực tế (1-2s) để không bỏ sót sự kiện ngắn |
| `K_ADAPT` | 0.01–0.02 | Chỉ áp dụng nếu triển khai bước 4 |

## 6. Checklist kiểm thử trước khi triển khai thực địa

- [ ] Test với bản ghi âm noise rừng thực tế (không có bawl) — đo tỷ lệ false positive trước/sau khi đổi cơ chế
- [ ] Test với bản ghi bawl trong môi trường yên tĩnh — đảm bảo `MIN_DURATION_MS` không làm tăng độ trễ phát hiện quá mức
- [ ] Test với bản ghi bawl trộn nền ồn (mô phỏng điều kiện thực tế rừng) — đo tỷ lệ recall (không bỏ sót)
- [ ] Đo thời gian phản hồi thực tế (từ lúc bawl bắt đầu đến lúc `triggerAlert()`) — so sánh với yêu cầu độ trễ cho ứng dụng cứu hộ
- [ ] Kiểm tra RAM/CPU overhead của state machine mới trên ESP32-S3 (không đáng kể vì chỉ thêm vài biến và 1 mảng nhỏ, nhưng vẫn nên đo)
