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
