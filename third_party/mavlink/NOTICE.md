# Nguồn thư viện MAVLink

- Kho upstream: https://github.com/mavlink/c_library_v2
- Commit cố định: `41874cfb2af7e658ca9ed88a13e91c750bcd2263`.
- Dialect sử dụng: `common`, cùng các header phụ thuộc `standard` và `minimal`.
- Nguồn và SHA256 archive được ghi trong `UPSTREAM.json`.

Các header upstream được giữ nguyên byte và giữ nguyên thông báo bản quyền trong từng tệp. Phần triển khai SHA-256 có thông báo và điều kiện phân phối riêng tại đầu `mavlink_sha256.h`; cần giữ nguyên thông báo đó khi sử dụng/phân phối.

Mã thư viện bên ngoài phục vụ tạo/đối chiếu dữ liệu chuẩn, không được tính là phần parser hoặc decoder tự triển khai của nhóm. Việc nâng phiên bản thư viện phải đi cùng cập nhật metadata và đối chiếu lại toàn bộ fixture.
