# Rubi firmware

Firmware cho bo màn hình ES3C28P (ESP32-S3, 2.8") của robot Rubi, xây dựng trên Xiaozhi (xiaozhi-esp32).

- Nền: bo `freenove-esp32s3-display-2.8-lcd` của Xiaozhi (cùng sơ đồ chân với ES3C28P).
- Ngôn ngữ hệ thống: tiếng Việt (vi-VN).
- Từ đánh thức tùy chỉnh: "Rubi" (đăng ký theo phiên âm `ru bi`).
- Cảm ứng: chạm 1 lần = bật/tắt trò chuyện, chạm 2 lần = bắt đầu nghe, giữ 3 giây = cấu hình Wi-Fi.

## Cách lấy file nạp

1. Vào tab **Actions** → **Build Rubi firmware** → **Run workflow**.
2. Khi chạy xong, mở lần chạy đó, tải **rubi-firmware** ở mục Artifacts, giải nén ra `merged-binary.bin`.
3. Nạp vào bo bằng Flash Download Tool: ESP32-S3, địa chỉ `0x0`.

Mã nguồn gốc được cố định ở commit ghi trong `UPSTREAM_COMMIT`. Các thay đổi riêng nằm trong thư mục `overlay/`.
