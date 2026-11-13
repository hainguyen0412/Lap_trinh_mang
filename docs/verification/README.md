# Kết quả kiểm thử bản tích hợp

Ngày 13/11/2026, bản Windows Release build thành công bằng CMake/Ninja và vượt qua **18/18 bài CTest** trên Windows x64, MSYS2 UCRT64, GCC 16.1.0. Thời điểm, mã SHA256 nguồn và thời gian từng bài được ghi trong [test_summary.json](test_summary.json).

Phạm vi gồm fixture, cấu hình JSON, socket/hàng đợi, parser/decoder/Inspector/logger, Command Manager, metrics, gateway UDP/TCP, ACK/retry, fake gateway và demo A+B+C ở B0/B1/B2. Bài abc_transport_integration đối chiếu đủ khung trên hai observer và một controller, heartbeat uplink, ACK cho ARM/TAKEOFF/LAND và dừng sạch.

Chạy lại tại gốc project:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

Chi tiết chạy được sinh trong build/windows-release/Testing và output/test_results, không đưa log hoặc executable vào Git. CI trên GitHub chạy lại độc lập; kết quả local này không thay thế trạng thái CI.

**Chưa kiểm thử PX4 SITL thực tế, QGroundControl hoặc bay.** Nguồn UDP trong kiểm thử localhost chỉ mô phỏng giao thức. Nhóm C tiếp tục theo [kế hoạch PX4](../px4-testing.md) và [mẫu báo cáo](../px4-results-template.md). Chưa thực hiện benchmark 70 giây theo tải thực.
