# Bộ dữ liệu mẫu MAVLink v1

`v1` là phiên bản của bộ fixture; khung giao thức trong bộ mẫu là MAVLink 2 không ký số.

`valid.bin` chứa 7 khung, tổng cộng 209 byte: HEARTBEAT, SYS_STATUS, ATTITUDE, GLOBAL_POSITION_INT, COMMAND_ACK, COMMAND_LONG và PING. Mỗi khung cũng có tệp `.bin` riêng.

| Tệp | Mục đích |
|---|---|
| `manifest.json` | Commit thư viện, seed, offset/độ dài, raw hex, header và giá trị giải mã mong đợi |
| `errors.json` và các binary lỗi | Sai CRC, msgid không hỗ trợ, chữ ký và incompat flags |
| `tcp_chunks.json` | Cách chia luồng để kiểm tra TCP |
| `udp_datagrams.json` | Ranh giới datagram và trường hợp bị cắt |
| `seq_cases.json` | SEQ quay vòng, thiếu, lặp và bước nhảy mơ hồ |
| Các mẫu trimmed/battery_unknown | Payload cắt đuôi giá trị 0 và pin không xác định |

`tools/generate_fixtures.cpp` dùng thư viện MAVLink C trực tiếp, độc lập với parser/decoder của dự án. `tests/verify_fixtures.py` đối chiếu CRC bằng thuật toán bitwise và giải mã bằng struct Python.

Dữ liệu trong các tệp là dữ liệu thử nghiệm cố định, không phải telemetry thu từ PX4.
