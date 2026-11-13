# Lập trình mạng — MAVLink Gateway

Khung dự án C++17 phục vụ đồ án lập trình mạng.

## Mục tiêu dự kiến

Nhận telemetry MAVLink qua UDP, xử lý bản tin và chuyển tiếp đến các client TCP; hỗ trợ một client điều khiển gửi lệnh và theo dõi ACK từ PX4 SITL.

## Cấu trúc dự kiến

```text
Lap_trinh_mang/
├── .github/workflows/       # Cấu hình CI
├── apps/                   # Điểm khởi chạy gateway và client
├── include/mavlink_gateway/ # Giao diện dùng chung
├── src/
│   ├── config/             # Cấu hình chung
│   ├── gateway/            # A: mạng và quản lý client
│   ├── processing/         # B: parser, decoder và Inspector
│   ├── logging/            # B: ghi log và thống kê
│   └── client/             # C: client và Command Manager
├── config/                 # Cấu hình mẫu
├── tests/
│   ├── unit/               # processing, gateway, client
│   ├── integration/        # Kiểm thử phối hợp
│   └── fixtures/v1/        # Dữ liệu mẫu và đáp án
├── tools/                  # Công cụ phát triển, kiểm thử
├── examples/               # Ví dụ sử dụng
├── scripts/windows/        # Script build, test và chạy
├── third_party/            # mavlink, nlohmann, licenses
└── docs/                   # diagrams, references, verification
```

## Tiến độ

- Đã khởi tạo cây thư mục dự án.
- Đợt 2: bổ sung thư viện MAVLink C cố định phiên bản, bộ tạo dữ liệu mẫu và phép đối chiếu độc lập.

- Đợt 3: đọc và kiểm tra cấu hình JSON chung; bổ sung thư viện JSON và kiểm thử cấu hình.

- Đợt 4: Winsock, hàng đợi giới hạn, quản lý socket RAII và xử lý gửi thiếu byte.

- Đợt 5: Tách khung MAVLink TCP UDP và kiểm tra CRC.

- Đợt 6: Bổ sung công cụ MAVLink và máy chủ giả lập.

- Đợt 7: Giải mã telemetry và theo dõi SEQ theo nguồn.

- Đợt 8: Ghi CSV bằng hàng đợi có giới hạn.

- Đợt 9: Chuyển tiếp telemetry UDP tới nhiều client TCP.

Các thư mục còn lại được giữ bằng `.gitkeep` để triển khai ở các đợt tiếp theo.

## Kiểm tra bộ mẫu

Tại gốc repository, dùng g++ hỗ trợ C++17 và Python 3.9+:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -isystem third_party/mavlink tools/generate_fixtures.cpp -o build/fixture_generator.exe
.\build\fixture_generator.exe output/fixtures_regenerated
python -B tests/verify_fixtures.py
python -B tests/verify_fixtures.py output/fixtures_regenerated
```

Chương trình tạo mẫu chạy độc lập, chưa cần module parser/decoder của dự án. Các lệnh trên chỉ kiểm tra bộ fixture; các module ứng dụng sẽ được bổ sung theo kế hoạch.

`build/` và `output/` dành cho dữ liệu sinh ra khi triển khai và chạy thử, được Git bỏ qua. Cấu hình riêng của máy dùng `config/*.local.json`.

## Kiểm tra cấu hình chung (đợt 3)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_config.ps1
```

Bộ đọc cấu hình từ chối khóa lạ, sai kiểu, cổng/ID/giới hạn không hợp lệ và chế độ ngoài B0/B1/B2. Mẫu nằm trong `config/gateway.example.json`; cấu hình riêng của máy có thể dùng đuôi `.local.json`.

Thư viện nlohmann/json 3.11.3 được lưu tại `third_party/nlohmann/json.hpp`, kèm giấy phép MIT trong `third_party/nlohmann/LICENSE`. Nguồn: https://github.com/nlohmann/json/tree/v3.11.3.

## Kiểm tra thành phần mạng cơ sở (đợt 4)

Chạy trên Windows với g++ MinGW-w64:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_network.ps1
```

Kiểm tra hàng đợi theo số phần tử/dung lượng, FIFO và đánh thức khi đóng; gửi thiếu byte, would-block, interrupt, hủy, lỗi socket và kết quả bất hợp lệ. Các ca socket thật dùng cổng localhost tạm để kiểm tra bind UDP/TCP, độc quyền cổng và quyền sở hữu socket.

Đợt này cung cấp các thành phần mạng cơ sở. Vòng xử lý gateway và chuyển tiếp nhiều client sẽ được triển khai ở các đợt sau.

Kiểm tra đợt 5: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_parser.ps1`.

Đợt 6: các công cụ `tools/fake_gateway.cpp`, `tools/replay_udp.cpp` và đóng gói MAVLink sử dụng parser đã có ở đợt 5. Kiểm tra hàng đợi bằng `scripts/test_send_queue.ps1`; kiểm tra server bằng `tests/integration/fake_gateway_test.py` sau khi build công cụ vào build/bin.

Kiểm tra đợt 7: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_decoder_inspector.ps1`.

Kiểm tra đợt 8: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_logging.ps1`.

Build gateway: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_gateway.ps1`. Kiểm tra chuyển tiếp: `python -B tests/integration/telemetry_fanout_test.py`. Mốc này chỉ chuyển telemetry; uplink được đóng để chờ kiểm tra vai trò ở đợt 10.
