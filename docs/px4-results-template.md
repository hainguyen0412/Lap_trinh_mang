# Kết quả kiểm thử PX4 — C điền

- Ngày giờ, người chạy:
- Project version và SHA256 `build/bin/gateway.exe`, `build/bin/c_client.exe`:
- PX4 version/commit, model SITL, lệnh khởi chạy:
- Windows/WSL/Linux version; số CPU logic và RAM:
- IP Windows/PX4/client, chế độ mạng, UDP endpoint thực tế:
- File cấu hình dùng và run-id:
- Mode, warmup, duration, số client, tải thực tế:

| Mục | PASS / FAIL / CHƯA CHẠY | Quan sát thực tế | Đường dẫn bằng chứng |
|---|---|---|---|
| Khóa nguồn HEARTBEAT và hiển thị telemetry | | | |
| 2 observer + 1 controller | | | |
| Giới hạn vai trò | | | |
| ARM: ACK + trạng thái PX4 | | | |
| TAKEOFF: ACK + độ cao PX4 | | | |
| LAND: ACK + hạ cánh PX4 | | | |
| Mất telemetry và heartbeat stale | | | |
| Ngắt/kết nối lại, dừng sạch | | | |
| B0/B1/B2, mỗi mode 3 lượt nếu đã đo | | | |

Nếu có lỗi: ghi các bước từ lúc khởi động, mong đợi gì, thực tế gì, thông báo đầy đủ, run-id và tệp log liên quan. Không sửa nội dung CSV gốc.

Đính kèm: JSON cấu hình, thư mục log của từng run, console PX4 và lệnh cấu hình MAVLink, ảnh/trạng thái PX4 cần đối chiếu.
