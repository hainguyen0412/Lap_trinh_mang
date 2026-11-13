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

Trạng thái: khởi tạo cấu trúc thư mục. Các thư mục được giữ trong Git bằng tệp `.gitkeep` rỗng.

`build/` và `output/` dành cho dữ liệu sinh ra khi triển khai và chạy thử, được Git bỏ qua. Cấu hình riêng của máy dùng `config/*.local.json`.
