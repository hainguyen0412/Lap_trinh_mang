# Phần C trong bản tích hợp

C sở hữu console client kết nối đến gateway, Command Manager, bộ replay/fake gateway dùng thử và việc kiểm chứng trên PX4 SITL. Client dùng lại parser, decoder, Inspector và logger của B; không duy trì một bản contracts hay parser riêng.

## Các tệp chính

| Tệp trong src/client | Trách nhiệm |
|---|---|
| `apps/client_main.cpp` | Điểm khởi chạy; ghép thành phần, nhận TCP, hiển thị, heartbeat, console lệnh và dừng/ghi kết quả |
| `client_config.hpp/.cpp` | Đọc JSON chung, kiểm tra CLI và tạo cấu hình hiệu lực |
| `net_socket.hpp/.cpp` | RAII socket TCP/UDP, send đủ byte, readiness receive và shutdown |
| `send_queue.hpp/.cpp` | Một worker ghi cho mỗi TCP socket, giới hạn hàng đợi, ưu tiên giữa các khung, drain/cancel |
| `mavlink_builder.hpp/.cpp` | Đóng gói HEARTBEAT GCS và COMMAND_LONG bằng MAVLink C |
| `command_manager.hpp/.cpp` | Một yêu cầu đang chờ, retry, kiểm tra nguồn/đích ACK, IN_PROGRESS và timeout |
| `tx_record.hpp` | ID và timestamp theo lần gửi; kết quả hoàn tất/cancel |
| `client_metrics.hpp/.cpp` | Bộ đếm theo cửa sổ đo, phân vị có giới hạn, CPU/RSS thực tế |
| `client_log.hpp/.cpp` | Adapter sử dụng CsvLogger B, log lệnh và file cấu hình/đo kèm theo |
| `periodic_sampler.hpp` | Lấy mẫu định kỳ độc lập với vòng recv, dừng có join |

## Các điểm đã sửa

1. Build chung bằng CMake, dùng lại parser/logger B và JSON loader chung. CLI có ưu tiên rõ ràng và bị từ chối khi sai kiểu/giới hạn; không đọc cấu hình phẳng cũ.
2. Callback dựng lệnh giữ cấu hình bằng giá trị để không giữ tham chiếu tới biến cục bộ đã hết vòng đời. Retry giữ tham số/target đúng, tăng confirmation và tạo lại SEQ cho từng lần.
3. B0 vẫn giải mã COMMAND_ACK cần cho controller, gồm ACK đến muộn. Chỉ nhận ACK đúng command/nguồn; target_system/target_component khác 0 đều phải khớp GCS. Giá trị 0 được coi là không chỉ định.
4. Một hàng đợi gửi với một writer tránh trộn byte heartbeat/lệnh/ACK. Frame ưu tiên không cắt ngang frame đang gửi. Fake gateway dùng timer riêng để không chặn nhận trong thời gian đợi ACK và gửi fixture theo ranh giới từng khung.
5. Có log cấu hình hiệu lực, định kỳ metrics khi socket nhàn rỗi, cửa sổ warmup/đo đúng, thống kê uplink tách downlink, CPU/RSS đo thật và latency có giới hạn bộ nhớ. Thời gian monotonic thuộc từng tiến trình; không dùng để đo xuyên máy.
6. Ctrl+C/quit được phát hiện ngay cả khi peer chưa đóng chiều gửi. Trước khi chốt log, dừng sampler/ticker, drain có hạn, hủy phần còn lại và join send worker. Lỗi ghi log được báo ra stderr và exit code lỗi.
7. Trên Windows, worker gửi dùng overlapped WSASend với token hủy. Khi dừng, thao tác đang chờ được hủy bằng CancelIoEx và được thu hồi hoàn tất trước khi giải phóng buffer. Không phụ thuộc riêng vào shutdown để đánh thức một send đang bị chặn; giữ nguyên giới hạn dừng một giây trong bài hồi quy. Bài cancel_write_without_shutdown kiểm tra việc hủy khi socket vẫn mở.

`commands.csv` dùng `queued` để nói lệnh đã được đưa vào hàng đợi. ACK timeout hiện bắt đầu tại thời điểm enqueue; khi hàng đợi quá chậm có thể hết thời gian trước lúc peer nhận được. B2 có sự kiện hoàn tất socket với timestamp riêng để đối chiếu. Chưa có kết quả PX4 thì không suy ra việc bay từ một ACK trong giả lập.

## Kiểm thử

`c_tests.exe` kiểm tra Command Manager bằng clock giả. `metrics_tests.exe` kiểm tra biên cửa sổ, dừng sớm, hạn drain và nearest-rank. `send_queue_tests.exe` kiểm tra luồng byte, ưu tiên, socket bị chặn, lỗi gửi và drain.

Các bài Python trong `tests/integration` mở TCP server tại localhost và đối chiếu raw/CRC/ACK/CSV độc lập. `command_retry_test.py`, `command_ack_test.py`, `client_config_test.py`, `fake_gateway_test.py`, `client_metrics_test.py` được gọi bởi TEST.cmd. `tools/local_demo.py` chạy gateway thật và client thật cùng một nguồn UDP giả lập, kiểm tra hai chiều ở B0/B1/B2.

Xem báo cáo xác minh trong `docs/verification`. Các lượt PX4 và benchmark theo tải thực cần C thực hiện theo `px4-testing.md`.
