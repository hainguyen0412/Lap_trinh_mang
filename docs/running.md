# Cách chương trình chạy

## 1. Khi nhấp file CMD

File CMD nằm trong `scripts/windows`. Nó lấy thư mục chứa chính nó bằng `%~dp0`, rồi gọi `..\run.ps1` ở thư mục scripts. PowerShell đặt thư mục làm việc về gốc project và gọi executable trong `build/bin`. Nhấp từ Explorer hoặc mở từ thư mục khác vẫn tìm được cấu hình và fixture.

Khi mới clone repo, cần build CMake theo README trước. Có thể nhấp `scripts/windows/BUILD.cmd` khi CMake, Ninja, g++ và Python đã trong PATH. ZIP Windows có binary sẵn có thể chạy demo trực tiếp.

PowerShell dùng `-ExecutionPolicy Bypass` cho đúng lần gọi đó để chạy script đi kèm, không sửa ExecutionPolicy toàn máy. C++ mới là chương trình thực hiện mạng và xử lý dữ liệu. Sau khi sửa C++, chạy BUILD để tạo lại `.exe`; sửa JSON thì chương trình đọc cấu hình ở lần khởi động tiếp theo.

## 2. Dễ nhất: demo tự động

Nhấp `scripts/windows/CHAY_DEMO.cmd`. Python chọn các cổng localhost tạm, ghi cấu hình riêng cho lần chạy, mở gateway thật và 2 observer + 1 controller thật. Khi client kết nối xong, một bộ giả lập UDP gửi HEARTBEAT/SYS_STATUS/ATTITUDE/POSITION. Gateway khóa endpoint khi nhận HEARTBEAT hợp lệ.

Demo lần lượt gửi ARM, TAKEOFF và LAND qua controller. Bộ giả lập trả ACK accepted để kiểm tra đường đi về. Demo đối chiếu lượng khung từng client nhận, kiểm tra heartbeat uplink, lưu báo cáo rồi đóng các tiến trình của lần chạy. Demo không phụ thuộc các cổng mặc định đang được project khác dùng và không dừng chương trình của bạn.

Nếu PASS, đường mạng A+B+C và ACK đã hoạt động trong kịch bản giả lập đó. Nếu FAIL, mở đường dẫn in ở cuối, đọc `result.json` và các file `.log`.

## 3. Chạy thủ công từng cửa sổ

Chạy gateway trước, chờ dòng `READY`. Nó bind UDP 14550 và hai cổng TCP 5760/5761, sau đó chờ dữ liệu.

Chạy observer: nó kết nối TCP 5760. Chưa có telemetry thì cửa sổ yên lặng là bình thường. Chạy controller: nó kết nối TCP 5761 và gửi heartbeat GCS định kỳ. Gateway chỉ chuyển heartbeat/lệnh đến PX4 sau khi khóa đúng nguồn và heartbeat nguồn còn mới.

C cho PX4 gửi telemetry UDP đến gateway. Nếu chưa có PX4, mở `scripts/windows/CHAY_MAU_UDP.cmd` để phát lại fixture 60 giây. Replay chỉ phát dữ liệu mẫu, không xử lý lệnh mới; ACK có sẵn trong fixture không chứng minh một lệnh thực tế đã được PX4 thực hiện. Kiểm thử lệnh hai chiều tại máy dùng CHAY_DEMO.

Controller hỗ trợ một lệnh chờ ACK mỗi lần. Ví dụ gõ `arm`, đợi ACK rồi gõ `takeoff 5`. Timeout mặc định 1 giây/lần, tối đa 3 lần gửi tổng cộng. ACK IN_PROGRESS chuyển sang chờ tiến độ theo timeout riêng. `quit` dừng controller; Ctrl+C dừng observer/gateway. Chờ chương trình ghi xong log trước khi đóng cửa sổ.

## 4. Cấu hình và lệnh cụ thể

Mở PowerShell tại thư mục project:

```powershell
# Dùng chung một ID để ghép log của ba cửa sổ.
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target gateway -RunId px4_01
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target observer -RunId px4_01 -Name observer_01
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target controller -RunId px4_01
```

Ba lệnh chạy ở ba cửa sổ khác nhau. Khi thử lại dùng ID mới, ví dụ px4_02. Muốn thêm observer thì đổi tên thành observer_02 để không ghi chung log. Client ở máy khác sửa `client.gateway_host` thành IP gateway. Trước khi chạy B2, sửa `measurement.mode` thành `B2` trong JSON chung; cả gateway và client mặc định đều lấy mode từ đó. Cũng có thể giữ JSON và chạy `scripts/windows/CHAY_GATEWAY.cmd B2`, `scripts/windows/CHAY_OBSERVER.cmd B2`, `scripts/windows/CHAY_CONTROLLER.cmd B2`, hoặc truyền `-Mode B2` vào run.ps1. Khi override mode gateway, script tạo một bản cấu hình hiệu lực trong output/configs, giữ nguyên file JSON gốc.

File JSON dùng chung có cấu trúc lồng; cấu hình phẳng cũ của A không còn dùng. Không sửa ID/cổng trong source. Có thể copy JSON thành cấu hình riêng rồi truyền `-Config config/ten_file.json`. Đường dẫn log tương đối được hiểu theo gốc project.

Kiểm tra cấu hình C sẽ dùng mà chưa kết nối mạng:

```powershell
.\build\bin\c_client.exe --config config/gateway.example.json --role controller --print-config
```

## 5. Các lỗi thường gặp

| Thông báo | Cách xử lý |
|---|---|
| `bind ... 10048` | Cổng đã có chương trình giữ. Dừng lần chạy gateway trước, hoặc đổi cả 3 cổng và cấu hình phía gửi/kết nối cho đồng bộ. Không cần chạy Administrator để chữa lỗi này. |
| `connect ... 10061` | Gateway chưa chạy/chưa READY, hoặc IP/cổng client chưa đúng. |
| `heartbeat_stale` | Endpoint đã khóa nhưng không còn HEARTBEAT đúng nguồn trong thời gian cho phép. Kiểm tra bên PX4 đang gửi đều. |
| `px4_not_known` | Chưa có HEARTBEAT hợp lệ đúng sysid/compid để khóa endpoint UDP. |
| Controller bị đóng ngay | Có controller khác đang kết nối hoặc config cổng/vai trò sai. |
| `Config/CLI error` | Đọc thông báo trường cụ thể; chương trình từ chối giá trị sai thay vì tự bỏ qua. |
| Không có log | Đọc đường dẫn thật được in ra; demo đặt log trong output/demos. Ghi chú mode B0/B1 không có log frames chi tiết. |

## 6. Đọc số liệu đúng

`valid_total` là tổng khung nhận hợp lệ từ lúc kết nối. `valid_window` chỉ tính khung nhận trong cửa sổ sau warmup. `sent_window` của C là số khung uplink đã được socket chấp nhận đầy đủ từ các lần enqueue trong cửa sổ và hoàn tất trước hạn drain; nó không phải số telemetry downlink. `pending_after_drain` phản ánh các lần gửi chưa có kết quả trước hạn đo. `not_sent_window` bao gồm phần không hoàn tất đúng hạn.

Observer không gửi uplink nên ô latency trống là đúng. B0 bỏ Inspector nên các trường tương ứng để trống. `latency_sampling_truncated=true` nghĩa là p95 lấy từ tập mẫu có giới hạn đã ghi rõ trong measurement.json. CPU chuẩn hóa theo số CPU logic thực tế; RSS đo từ tiến trình. `log_drop` và `log_truncated` cho biết log đã thiếu dữ liệu, cần tính đến khi kết luận.
