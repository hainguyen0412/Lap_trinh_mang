# Phụ thuộc đã lưu trong repository

| Vị trí | Nội dung |
|---|---|
| `mavlink` | MAVLink C v2, common/minimal/standard và header hỗ trợ |
| `nlohmann/json.hpp` | nlohmann/json 3.11.3 |
| `nlohmann/LICENSE` | Giấy phép MIT của thư viện JSON |
| `licenses` | Thông báo và giấy phép toolchain/runtime đi kèm binary Windows |

MAVLink khóa commit `41874cfb2af7e658ca9ed88a13e91c750bcd2263`; xem `mavlink/UPSTREAM.json` để biết nguồn và hash archive. Header upstream giữ nguyên. JSON lấy từ tag v3.11.3; nguồn cố định được ghi trong `licenses/NOTICE.md`.

Không coi mã thư viện là phần tự triển khai của sinh viên. Khi thay phụ thuộc, cập nhật metadata, giấy phép, fixture và đối chiếu độc lập trước khi chạy lại kiểm thử.
