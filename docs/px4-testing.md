# Bàn giao C kiểm thử PX4 SITL

Phạm vi bàn giao này là mã Windows của gateway và client; PX4 SITL/Gazebo chạy trên Ubuntu. Chưa thực hiện lượt PX4 trong đợt đóng gói. Hiện một laptop dual boot chưa chạy đồng thời được cả hai phía; có thể dùng thêm máy Windows nối mạng, hoặc chờ bản gateway Linux. Khi kiểm thử, ghi phiên bản/commit và cấu hình mạng thực tế vào mẫu kết quả.

## Chuẩn bị

1. Chạy `scripts/windows/TEST.cmd` trên máy Windows để xác nhận bộ kiểm thử nội bộ.
2. Copy cấu hình chung thành `config/px4.local.json`. Điền IP gateway trong `client.gateway_host`, xác nhận PX4 sysid/compid, GCS ID và ba cổng. Giữ MAVLink 2 common không ký số theo phạm vi project.
3. Tại PX4, cấu hình một đường MAVLink gửi UDP đến **IP gateway Windows:cổng udp_bind**. Lệnh thiết lập cụ thể tùy phiên bản/môi trường PX4 của C; chép nguyên lệnh thực tế vào báo cáo.
4. Trước mỗi lượt, dùng run-id mới và cùng run-id cho gateway và các client. Đảm bảo không có controller cũ giữ cổng.

## Thứ tự kiểm tra

| Mục | Thao tác | Bằng chứng cần giữ |
|---|---|---|
| Telemetry | Gateway READY → bật PX4 → mở observer | `px4_locked`, endpoint đúng, HEARTBEAT/ATTITUDE/POSITION hiển thị đúng đơn vị |
| Nhiều client | Mở 2–9 observer và 1 controller | Tất cả nhận telemetry; client chậm không làm client khác dừng |
| Vai trò | Thử controller thứ hai, thử uplink từ observer bằng bài kiểm thử có sẵn | Gateway từ chối đúng vai trò, ghi sự kiện |
| ARM | Từ controller gửi `arm` khi SITL đủ điều kiện | `commands.csv` + ACK và trạng thái armed từ PX4; ghi nguyên lý do nếu PX4 từ chối |
| TAKEOFF | Sau ARM thành công, gửi `takeoff 5` | ACK + thay đổi độ cao/trạng thái từ PX4 |
| LAND | Gửi `land` và theo dõi | ACK + trạng thái hạ cánh từ PX4 |
| ACK/retry | Ghi lại ACK accepted/rejected/in_progress nếu PX4 phát; lỗi timeout kiểm thử cục bộ có sẵn | Command ID, target, số lần gửi, thời điểm và kết quả; không gán timeout thành thành công |
| Mất nguồn | Dừng telemetry quá heartbeat_stale_after_ms rồi thử lệnh; bật lại cùng endpoint | `heartbeat_stale`, không gửi lệnh khi nguồn cũ; heartbeat cùng endpoint phục hồi |
| Dừng/mở lại | Dừng C rồi kết nối lại; dừng gateway bằng Ctrl+C | Không treo; log chốt và cổng được giải phóng |

ACK accepted chỉ chứng minh bên đích chấp nhận lệnh. Kết quả ARM/cất cánh/hạ cánh cần xác nhận bằng trạng thái và telemetry PX4.

## So sánh B0/B1/B2 sau kiểm thử chức năng

Giữ cùng build, cấu hình tải, số client, máy và mạng. Chỉ thay mode đồng bộ cho gateway/client. Dùng warmup 10 giây và cửa sổ 60 giây theo cấu hình; chạy ít nhất 3 lượt mỗi mode và ghi tải thực tế PX4 phát. Dữ liệu fixture replay lặp lại có SEQ lặp/nhảy không dùng để kết luận tỷ lệ mất gói PX4.

Gửi lại: mẫu `px4-results-template.md` đã điền, JSON cấu hình dùng thực tế, toàn bộ thư mục log run tương ứng ở cả gateway/client, console PX4/lệnh khởi chạy và phiên bản PX4. Để nguyên log gốc; thêm nhận xét ở báo cáo. Kết quả mỗi mục ghi PASS/FAIL/CHƯA CHẠY, kèm cách tái hiện lỗi.
