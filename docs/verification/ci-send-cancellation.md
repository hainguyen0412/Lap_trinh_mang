# Sửa lỗi dừng luồng gửi trên GitHub Actions

[Lần chạy trên main trước khi sửa](https://github.com/hainguyen0412/Lap_trinh_mang/actions/runs/43426177764) build thành công nhưng chỉ qua 17/18 bài CTest. Trong send_queue_tests, stop_blocked_send báo `Worker did not finish within one second`; hai khung còn trong hàng đợi đã bị hủy nhưng khung đang gửi chưa hoàn tất.

Worker trước đây gọi blocking send và dựa vào shutdown để đánh thức nó. Bản sửa truyền token dừng vào lớp socket. Trên Windows, lớp này dùng WSASend overlapped, kiểm tra token trong lúc chờ và gọi CancelIoEx cho đúng thao tác gửi đó. Chờ xác nhận thao tác đã kết thúc trước khi giải phóng buffer và WSAOVERLAPPED; không đóng socket đồng thời với send/recv.

Giữ nguyên yêu cầu dừng dưới một giây. Thêm cancel_write_without_shutdown: tắt send buffer, giới hạn receive buffer, để một khối 8 MiB ở trạng thái đang gửi rồi hủy bằng token mà không shutdown socket. Cả 8/8 tình huống transport đã qua trong 5 lần chạy liên tiếp tại máy phát triển. Toàn bộ CTest và dấu vân tay mã nguồn của lần xác minh mới nằm trong [test_summary.json](test_summary.json).

Tham khảo API chính thức: [WSASend](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend), [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex). Trạng thái CI thực tế phải xem tại tab Actions của commit tương ứng; báo cáo local không thay thế CI.
