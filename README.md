# MAVLink Gateway — Lap_trinh_mang

Dự án lập trình mạng C++17: nhận telemetry MAVLink 2 qua UDP, kiểm tra khung rồi chuyển nguyên byte đến nhiều client TCP. Một controller gửi lệnh qua gateway đến PX4 và theo dõi ACK; các observer nhận telemetry để quan sát.

**Nền tảng hiện tại: Windows 64 bit, MinGW-w64/MSYS2 UCRT64.** PX4 SITL và Gazebo có thể chạy trên máy Ubuntu khác rồi nối qua mạng. Một laptop dual boot chưa chạy đồng thời được hai phía; bản gateway Linux sẽ được xử lý riêng sau.

## Cấu trúc

```text
Lap_trinh_mang/
├── .github/workflows/       # CI Windows
├── apps/                   # gateway_main.cpp, client_main.cpp
├── include/mavlink_gateway/ # API chung: contracts/config/processing/logger
├── src/
│   ├── config/             # JSON chung
│   ├── gateway/            # A: mạng, client, hàng đợi và định tuyến
│   ├── processing/         # B: parser, decoder, Inspector SEQ
│   ├── logging/            # B: CSV logger
│   └── client/             # C: console, lệnh, ACK và metrics
├── config/                 # Cấu hình mẫu; *.local.json được Git bỏ qua
├── tests/
│   ├── unit/               # processing/gateway/client
│   ├── integration/        # UDP/TCP và ACK tại localhost
│   └── fixtures/v1/        # Gói tin cùng đáp án chuẩn
├── tools/                  # Fixture generator, fake gateway, replay, demo, đóng gói
├── examples/               # Demo B đọc dữ liệu từ file
├── scripts/                # build.ps1, test.ps1, run.ps1, package.ps1
│   └── windows/            # Các file CMD để nhấp chạy
├── third_party/            # MAVLink C, JSON và giấy phép
├── docs/                   # Kiến trúc, hướng dẫn và bàn giao PX4
├── CMakeLists.txt
└── CMakePresets.json
```

`build/` và `output/` tự sinh khi build/chạy và được Git bỏ qua. Executable nằm trong `build/bin`; CSV và kết quả thử nằm trong `output`.

## Build lần đầu

Cần CMake **3.25+**, Ninja, g++ MinGW-w64 UCRT64 và Python **3.9+** trong PATH. Không cần pip package cho project; MAVLink/JSON đã nằm trong `third_party`.

Nếu dùng MSYS2, mở terminal **UCRT64** để cài công cụ:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python
```

Tại gốc project, chạy trong UCRT64:

```bash
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

Nếu dùng PowerShell, đưa công cụ UCRT64 vào PATH của cửa sổ đó (đổi đường dẫn nếu MSYS2 cài ở vị trí khác):

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1
```

Preset `windows-debug` dùng để debug; script nhận `-Configuration Debug`. `test.ps1 -UnitOnly` chỉ chạy unit tests; `test.ps1 -Build` build rồi chạy toàn bộ. Các preset dùng chung executable `build/bin`; build cấu hình muốn chạy trước khi test cấu hình đó.

## Demo A+B+C tại máy

Sau khi build, nhấp [scripts/windows/CHAY_DEMO.cmd](scripts/windows/CHAY_DEMO.cmd), hoặc chạy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target demo -Mode B2
```

Demo chọn cổng localhost tạm, mở gateway thật và 2 observer + 1 controller, phát telemetry UDP giả lập, kiểm tra ARM/TAKEOFF/LAND và ACK rồi tự đóng các tiến trình của lần chạy. Nó in đường dẫn kết quả/log. Đây là kiểm thử giao thức giả lập; kết quả PX4 SITL được bổ sung sau.

## Chạy từng phần

Mở ba cửa sổ PowerShell tại gốc project; mỗi cửa sổ chạy một lệnh:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target gateway -RunId px4_01
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target observer -RunId px4_01 -Name observer_01
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run.ps1 -Target controller -RunId px4_01
```

Cổng mặc định: UDP **14550**, TCP observer **5760**, TCP controller **5761**. Dùng run-id mới khi chạy lại. Các file `CHAY_GATEWAY.cmd`, `CHAY_OBSERVER.cmd`, `CHAY_CONTROLLER.cmd` nằm trong `scripts/windows`; khi nhấp chạy, chương trình tự tạo run-id riêng và in đường dẫn log.

Cấu hình chung ở [config/gateway.example.json](config/gateway.example.json). Copy thành `config/px4.local.json` để thử riêng rồi truyền `-Config config/px4.local.json`. Client cùng máy gateway giữ host `127.0.0.1`; client máy khác dùng IP gateway. PX4 gửi UDP đến IP Windows và cổng đã cấu hình; gateway khóa endpoint nguồn từ HEARTBEAT hợp lệ.

Controller hỗ trợ `arm`, `disarm`, `takeoff`, `land`, `help`, `quit`. Độ cao TAKEOFF hiện được chuyển trực tiếp vào param7 của COMMAND_LONG; phải đối chiếu phiên bản PX4 trước khi diễn giải độ cao bay. Kiểm tra cả ACK lẫn trạng thái PX4. Dừng observer/gateway bằng Ctrl+C để hoàn tất log.

## Chế độ và log

| Mode | Hoạt động |
|---|---|
| B0 | Parser/CRC, mạng, định tuyến và đo tối thiểu; controller vẫn xử lý ACK |
| B1 | Thêm decoder và Inspector SEQ |
| B2 | Thêm log chi tiết theo khung và lần gửi |

Chạy thủ công lưu `output/runs/<run_id>/<component>/`; chọn tên thật chương trình in ra. Demo lưu dưới `output/demos`. `metrics.csv` lấy mẫu định kỳ; `summary.csv` chốt cửa sổ đo; B2 có `frames.csv`, `issues.csv` và sự kiện chi tiết theo dữ liệu phát sinh.

`queued` là đã xếp hàng, `sent_to_socket` là socket chấp nhận đủ khung, `ack_accepted` là đích chấp nhận lệnh. Timestamp monotonic chỉ so sánh trong cùng tiến trình. Mặc định warmup 10 giây và đo 60 giây; đây là cửa sổ thống kê, không tự đóng client.

## GitHub và phát hành

`.github/workflows/windows-ci.yml` cấu hình build CMake và chạy CTest trên Windows khi push hoặc mở pull request. Kết quả CI thực tế được xem trong tab Actions sau khi workflow được đẩy lên GitHub.

Không commit `build/`, `output/`, executable, cache, `*.local.json` hoặc `.env`. Fixture `.bin`, thư viện đã khóa phiên bản và tài liệu thiết kế là đầu vào cần giữ trong Git.

Đóng gói mã nguồn:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

Thêm `-WithBinaries` để tạo ZIP Windows có executable sau khi build. ZIP/SHA256 nằm trong `build/releases`; mỗi ZIP có manifest kiểm tra file. Bản Windows có thể chạy demo khi có Python; muốn chạy CTest thì configure/build CMake tại vị trí mới. Có thể đính kèm ZIP Windows trong GitHub Releases.

Trong ZIP đã giải nén, chạy `python -B tools/verify_package.py` để đối chiếu manifest. Repo nguồn không giữ manifest phát hành cố định vì file thay đổi theo từng commit.

## Tài liệu

- [Kiến trúc và phân công](docs/architecture.md)
- [Hướng dẫn chạy](docs/running.md)
- [Giao ước tích hợp](docs/integration-contract.md)
- [Phần B](docs/part-b.md), [phần C](docs/part-c.md)
- [Kế hoạch PX4](docs/px4-testing.md), [mẫu báo cáo](docs/px4-results-template.md)
- [Xác minh bản tổ chức lại](docs/verification/README.md)
- [Thư viện và giấy phép](third_party/README.md)

Build/test dùng [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) và [CTest](https://cmake.org/cmake/help/latest/module/CTest.html). Workflow dùng [MSYS2 UCRT64](https://github.com/msys2/setup-msys2) để thống nhất toolchain Windows.

## Thành viên

- A: Phạm Thị Hồng Nhung — Nhung106230021: gateway, mạng và định tuyến.
- B: Nguyễn Đức Hải — hainguyen0412: parser, Inspector, CSV logger và tích hợp build.
- C: Đinh Tiến Trung — Trungdinh0565: client, Command Manager và kiểm thử tích hợp.

Các kiểm thử localhost dùng dữ liệu mô phỏng giao thức; kiểm thử PX4 SITL thực tế còn chờ nhóm thực hiện theo docs/px4-testing.md.
