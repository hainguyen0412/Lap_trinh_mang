# Phụ thuộc

`../nlohmann/json.hpp`: nlohmann/json 3.11.3, Niels Lohmann và cộng tác viên, giấy phép MIT trong `../nlohmann/LICENSE`.

Nguồn cố định phiên bản: https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

Thư viện được giữ để build offline. Mã mạng A nằm trong `src/gateway`, chương trình khởi động ở `apps`, kiểm thử trong `tests`; mã JSON không được tính là phần tự triển khai.

Binary phân phối được build bằng MinGW-w64/MSYS2 GCC, liên kết runtime tĩnh. GCC Runtime Library Exception cho phép phân phối chương trình đã liên kết theo điều kiện của exception; bản giấy phép đi kèm được lưu trong thư mục này khi đóng gói.
