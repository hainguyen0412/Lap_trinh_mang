# Hướng dẫn triển khai và bàn giao phần B

## 1. Phần đã có

| Thành phần | Tệp | Công việc |
|---|---|---|
| Giao ước chung | `include/mavlink_gateway/contracts.hpp` | Kiểu đầu vào/đầu ra, raw frame, decoder và snapshot |
| API xử lý của B | `include/mavlink_gateway/processing.hpp` | Các lớp `Parser`, `Decoder`, `Inspector` |
| Xử lý MAVLink | `src/processing/processing.cpp` | Tách khung, CRC, phân loại, giải mã 5 bản tin, SEQ và snapshot |
| API Logger | `include/mavlink_gateway/csv_logger.hpp` | Cấu hình Logger, ghi bản ghi typed hoặc theo tên cột |
| Logger | `src/logging/csv_logger.cpp` | Hàng đợi/thread riêng, CSV, B0/B1/B2, giới hạn file/run, metadata và summary |
| Bộ tạo mẫu độc lập | `tools/generate_fixtures.cpp` | Dùng thư viện MAVLink C tham chiếu; không gọi parser/decoder của B |
| Mẫu và đáp án | `tests/fixtures/v1/` | Binary, raw hex, manifest, lỗi, chia TCP và ranh giới UDP |
| Kiểm thử C++ | `tests/unit/processing/processing_tests.cpp` | U01-U11 và các ca bổ sung cho giới hạn, thread, Logger |
| Đối chiếu Python | `tests/verify_fixtures.py` | CRC bitwise và giải mã trường bằng struct, không phụ thuộc module C++ |
| Demo | `examples/parser_demo.cpp` | Đọc file, giả lập từng đoạn TCP/datagram UDP, in dữ liệu và xuất log |

Thư viện tham chiếu đặt ở `third_party/mavlink/`, khóa commit `41874cfb2af7e658ca9ed88a13e91c750bcd2263`; nguồn và hash archive được ghi trong `UPSTREAM.json`. Không tải mạng khi build hoặc chạy. Các header upstream giữ nguyên; adapter dùng metadata/CRC và decoder do thư viện cung cấp, còn phần tách luồng và phân loại theo phạm vi dự án nằm trong `Parser` của B.

## 2. Build và chạy

Build chung bằng CMake/Ninja và GCC MinGW UCRT64 trên Windows, C++17. Mở PowerShell tại gốc project với các công cụ và Python 3.9+ trong PATH:

```powershell
# Build, kiểm tra fixture độc lập và chạy test C++.
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Build -UnitOnly

# Chỉ build lại.
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug

# Chạy demo bằng dữ liệu đã có, TCP chia 7 byte/lần.
.\build\bin\b_demo.exe .\tests\fixtures\v1\valid.bin B2 tcp

# Thử chế độ ít chức năng hơn hoặc xử lý một datagram UDP.
.\build\bin\b_demo.exe .\tests\fixtures\v1\valid.bin B0 udp
.\build\bin\b_demo.exe .\tests\fixtures\v1\valid.bin B1 tcp

# Chủ động tạo lại fixture khi đã kiểm tra thay đổi phiên bản tham chiếu.
.\build\bin\fixture_generator.exe .\tests\fixtures\v1
python .\tests\verify_fixtures.py
```

Build tạo `libmgw_processing.a` và `libmgw_logging.a` trong `build/windows-release/lib` hoặc thư mục Debug tương ứng. Executable nằm trong `build/bin`. Toolchain hỗ trợ hiện tại là GCC MinGW UCRT64; chưa kiểm chứng MSVC hoặc gateway Linux.

Demo mặc định đọc `valid.bin`, B1, TCP. Đối số lần lượt là `[input.bin] [B0|B1|B2] [tcp|udp] [unique_run_id]`. Nếu không truyền run ID, demo tự tạo ID riêng. Logger từ chối ghi đè một thư mục run/component đã có dữ liệu.

Mẫu chính gồm 7 khung, 209 byte: HEARTBEAT, SYS_STATUS, ATTITUDE, GLOBAL_POSITION_INT, COMMAND_ACK, COMMAND_LONG và PING. Dữ liệu ví dụ: pin 12,3 V/85%, roll 0,1 rad, pitch -0,2 rad, yaw 1,5 rad; vị trí 21 độ/105,8 độ; độ cao MSL 123,456 m và tương đối 5 m. COMMAND_LONG/PING hợp lệ vẫn được parser xuất raw nhưng không nằm trong 5 loại decoder.

Kết quả kiểm thử hiện có 19 nhóm, gồm kiểm tra adapter Logger cho gateway. Các số trong demo là giá trị từ fixture, không phải telemetry nhận từ drone thật hay số đo tốc độ mạng.

## 3. A gọi module B như thế nào

Ví dụ này nằm trong thread xử lý của gateway, sau khi A đã nhận byte vào một `ReceiveEvent` có source/session hợp lệ. A tạo các đối tượng một lần, không tạo lại parser cho từng recv:

```cpp
#include "mavlink_gateway/processing.hpp"
#include "mavlink_gateway/csv_logger.hpp"

mavlink_gateway::Parser parser;
mavlink_gateway::Decoder decoder;
mavlink_gateway::Inspector inspector;
std::uint64_t next_frame_id = 1;

// Trong vòng xử lý của worker sở hữu parser:
auto batch = parser.consume(event);
for (const auto& issue : batch.issues) {
    // B1/B2: inspector.on_issue(issue);
    // B2: logger.record_issue(issue);
}
for (auto& frame : batch.frames) {
    frame.frame_id = next_frame_id++;
    // B1/B2:
    auto decoded = decoder.decode(frame);
    inspector.on_frame(frame, decoded);
    // B2: logger.record_frame(frame);
    auto immutable = std::make_shared<const mavlink_gateway::FrameRecord>(std::move(frame));
    // A kiểm tra vai trò/nguồn và đưa immutable vào từng hàng đợi gửi phù hợp.
}
if (batch.close_tcp_session) {
    // A đóng session hiện tại, giải phóng hàng đợi/socket theo cơ chế phần mạng.
}
```

Ví dụ trên minh họa luồng B1/B2. B0 bỏ decoder và Inspector, nhưng gateway vẫn cần đọc tối thiểu các trường cần cho khóa nguồn và kiểm tra uplink. Parser vẫn chạy CRC trong mọi chế độ. `next_frame_id` phải được cấp từ một bộ cấp ID chung/đồng bộ nếu A có nhiều worker tạo khung.

Mỗi `Parser` được sở hữu bởi đúng một worker; bộ đệm bên trong phân biệt source ID. Lần nhận TCP có thể chưa xuất khung, có thể xuất nhiều khung. Khi kết nối đóng, worker gọi `parser.reset(source_id)` và `inspector.forget_source(source_id)`; counters tích lũy của Inspector được giữ, còn dữ liệu nguồn đã đóng được giải phóng. Phiên mới dùng ID mới.

Khi worker gửi của A hoàn tất hoặc bỏ một khung, A tạo `DeliveryRecord` và gọi `logger.record_delivery()`. Mốc `send_done_ns` chỉ có khi đã gửi đủ raw vào socket. B không tự tạo mốc gửi, số client, CPU/RAM hay kết quả socket khi không có phần mạng cung cấp.

Thêm `include/` vào include path công khai, `third_party/mavlink/` làm system/private include path khi biên dịch phần B, biên dịch `src/processing/processing.cpp` và `src/logging/csv_logger.cpp`, liên kết hỗ trợ thread của toolchain. Public header không chứa kiểu Winsock hay kiểu MAVLink C.

## 4. C dùng lại phần B

C dùng `Parser` ở mỗi worker nhận TCP của client, sau đó `Decoder` để hiển thị dữ liệu. COMMAND_ACK đã được giải mã cung cấp cho Command Manager của C. B không gửi lệnh, không quản lý timeout/gửi lại lệnh và không kiểm chứng trạng thái bay thay C.

Thời gian `ReceiveEvent.received_at_ns` dùng một mốc steady_clock chung trong chính tiến trình client. Không lấy timestamp client trừ trực tiếp timestamp gateway. Các ID trong client là cục bộ, không phải frame ID của gateway.

## 5. Log và B0/B1/B2

`CsvLogger` tự thêm run ID/component vào các dòng. Có ba hàm typed cho frame, parse issue và delivery. Dùng `record_fields(table, map)` cho Commands, Events và Metrics; tên cột không có trong schema sẽ báo lỗi, trường thiếu để ô trống. Không dùng 0 thay cho số liệu chưa đo được.

```cpp
mavlink_gateway::LoggerOptions options;
options.run_id = "trial_001";
options.component = "gateway";
options.mode = mavlink_gateway::MeasurementMode::B2;
mavlink_gateway::CsvLogger logger(options);

logger.record_fields(mavlink_gateway::LogTable::Events,
    {{"event_ns", "1000"}, {"event", "client_connected"}, {"session_id", "1"}});
logger.set_summary({{"clients", "2"}, {"valid_window", "12000"}});
// Dừng các producer trước khi gọi close.
logger.close();
auto status = logger.stats(); // Kiểm tra dropped, log_truncated và last_error.
```

Ví dụ summary là cách gọi API; các giá trị phải được thay bằng số đo thực tế khi dùng. Bộ test kiểm tra quote dấu phẩy, dấu nháy và xuống dòng; CSV giữ UTF-8 và mỗi file có một header. Hàng đợi chỉ chứa bản ghi, worker riêng thực hiện I/O, flush định kỳ mặc định 1 giây.

- B0/B1 vô hiệu log chi tiết Frames/Issues/Delivery; các lời gọi đó tăng `disabled`, không tính `dropped`.
- B2 bật các bảng chi tiết. CSV chỉ được tạo khi có bản ghi đầu tiên, nên không có lỗi thì có thể chưa có `issues.csv`.
- Đầy queue hoặc record quá 16 KiB: bỏ record mới, tăng dropped. Mặc định queue 10.000 record.
- Chạm file/run budget: ngừng ghi CSV tiếp theo, đánh dấu `log_truncated`, giữ 4 KiB cho metadata/summary cuối. Giá trị mặc định 100 MiB/file, 512 MiB/component/run.
- `stats.submitted = stats.written + stats.dropped` sau khi đóng, đối với các bảng được bật và không có lỗi flush. `last_error` phải được kiểm tra nếu I/O thất bại.
- Khi close, worker drain queue theo ngân sách thời gian mặc định 2 giây rồi bỏ phần còn lại. Đây là giới hạn vòng drain; std::ofstream không cung cấp cơ chế hủy cứng một lần ghi bị OS chặn.

`run.json` ghi mode, seed, phiên bản MAVLink, các giới hạn Logger thực tế và metadata caller cung cấp. A/C phải bổ sung phiên bản chương trình/PX4, phần cứng, mạng, cấu hình gateway đã resolve và endpoint PX4 khi tích hợp. Demo đánh dấu `scope=offline_demo`, network=none và PX4=not_used.

Demo B đọc tham số dòng lệnh và tạo LoggerOptions trực tiếp; gateway/client dùng JSON chung qua `src/config/config.cpp`.

## 6. Giới hạn cần hiểu khi bàn giao

- Parser chỉ chấp nhận profile v2 header cổ điển 10 byte và khung không ký. Các incompatible flag khác bị từ chối; khi không biết bố cục, parser bỏ marker và quét lại. Không coi header mở rộng mà thư viện mới có thể hiểu là được sản phẩm hỗ trợ.
- CRC sai tái đồng bộ bằng cách dịch một byte và quét tiếp. Nếu trường chiều dài bị hỏng thành một khung chưa đủ, TCP chờ thêm dữ liệu, UDP loại phần ứng viên bị cắt. Không khẳng định phục hồi mọi khung phía sau mọi dạng hỏng header.
- Các ngưỡng lỗi là bộ đếm ứng viên quan sát được, không phải bộ đếm chính xác mọi gói vật lý đã được gửi. SEQ gap chỉ là ước lượng có giả thiết; không cộng với CRC lỗi để kết luận tổng mất gói.
- Parser/decoder không xác nhận vai trò controller, danh sách lệnh, nguồn PX4 hoặc đích uplink. A áp dụng chính sách này sau kiểm tra khung trong bản gateway đã ghép.
- `Decoder` nhận FrameRecord đã được parser chấp nhận; không thay thế việc kiểm tra CRC của parser.
- Đã có kiểm thử B độc lập, U12 của A, các ca mạng localhost và một lượt tải B0 với 5 client. B0/B1/B2 đã được kiểm tra chức năng khi ghép A+B. Chưa có ba lượt hiệu năng mỗi chế độ hoặc kiểm chứng giao dịch lệnh và trạng thái bay trên SITL; các phần đó vẫn cần C.

## 7. Thứ tự bạn nên đọc mã

1. `contracts.hpp`: hiểu ReceiveEvent, FrameRecord và ParseBatch.
2. `Parser::consume()` và `Parser::extract()`: xem nơi giữ byte TCP và kiểm tra CRC.
3. `Decoder::decode()`: xem cách chuyển payload sang dữ liệu hiển thị.
4. `Inspector::on_frame()`: theo dõi cách cập nhật SEQ.
5. `examples/parser_demo.cpp`: xem cách nối ba bước vào một vòng xử lý.
6. `tests/unit/processing/processing_tests.cpp`: đối chiếu từng hành vi với dữ liệu kiểm thử.
7. Logger và giao ước log sau khi đã hiểu đường dữ liệu chính.

Tài liệu tham chiếu chính: [serialization](https://mavlink.io/en/guide/serialization.html), [thư viện C](https://mavlink.io/en/mavgen_c/), [common messages](https://mavlink.io/en/messages/common.html). Định nghĩa trường thực tế được đối chiếu với các header đã khóa commit trong third_party, không phụ thuộc một bản online có thể thay đổi.

## 8. Trạng thái bản ghép A+B

Phần A hiện dùng trực tiếp header chung, Parser/Decoder/Inspector và CsvLogger của B. Header contracts.hpp của B được giữ nguyên; không có bản sao bên A. Gateway đọc JSON chung tại config/gateway.example.json.

Logger có thêm bảng ClientSummary và API record_row để nhận bản ghi từ A mà vẫn kiểm tra số cột. Khi write_control_files=false, Gateway chịu trách nhiệm ghi run.json/summary.csv sau close; phần 64 KiB được giữ lại cho hai file này. Các chương trình B độc lập vẫn dùng mặc định write_control_files=true.

Chạy scripts/test.ps1 từ thư mục gốc để kiểm tra bản ghép, bao gồm B0/B1/B2, source/session và client chậm.
