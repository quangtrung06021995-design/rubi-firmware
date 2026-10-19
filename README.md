# Rubi firmware

Firmware cho bo màn hình ES3C28P (ESP32-S3, 2.8") của robot Rubi, xây dựng trên Xiaozhi (xiaozhi-esp32).

- Nền: bo `freenove-esp32s3-display-2.8-lcd` của Xiaozhi (cùng sơ đồ chân với ES3C28P).
- Ngôn ngữ hệ thống: tiếng Việt (vi-VN).
- Từ đánh thức: "Hey Rubi" (`hei ru bi`, kèm các biến thể `hai ru bi`, `hei lu bi`, `hai lu bi`, `hei ru bei`) và "Rubi ơi" (`ru bi ei`).
- Cảm ứng: chạm 1 lần = bật/tắt trò chuyện, chạm 2 lần = bắt đầu nghe, giữ 3 giây = cấu hình Wi-Fi.

## Cách lấy file nạp

1. Vào tab **Actions** → **Build Rubi firmware** → **Run workflow**.
2. Khi chạy xong, mở lần chạy đó, tải **rubi-firmware** ở mục Artifacts, giải nén ra `merged-binary.bin`.
3. Nạp vào bo bằng Flash Download Tool: ESP32-S3, địa chỉ `0x0`.

Mã nguồn gốc được cố định ở commit ghi trong `UPSTREAM_COMMIT`. Các thay đổi riêng nằm trong thư mục `overlay/`.

## Lưu ý quan trọng về từ đánh thức

Từ đánh thức và mô hình nhận giọng nằm trong phân vùng **assets** (0x800000), không nằm trong app.
Cập nhật OTA chỉ thay app, nên **mỗi khi đổi từ đánh thức phải nạp lại `merged-binary.bin` qua cáp USB** (địa chỉ `0x0`).
