# Giao ước tích hợp MAVLink Gateway phiên bản 1

Tài liệu này chốt giao diện và cấu hình chung cho A, B và C. Dự án Lap_trinh_mang đã tích hợp gateway, parser/Inspector/logger và client/Command Manager. Kiểm thử PX4 SITL sẽ do C thực hiện theo `px4-testing.md`. Các giá trị tải và timeout là tham số thử nghiệm, có thể thay bằng cấu hình và phải được ghi lại khi đo.

Hai tệp dùng chung:

- `include/mavlink_gateway/contracts.hpp`: kiểu dữ liệu C++17 và giao diện parser, decoder, Inspector.
- `config/gateway.example.json`: cấu hình mẫu đọc bởi gateway và công cụ/client. Mỗi chương trình chỉ sử dụng phần cấu hình liên quan.

## 1. Quyết định kỹ thuật

| Nội dung | Phương án chốt |
|---|---|
| Ngôn ngữ | C++17; mạng Windows dùng Winsock |
| Giao thức | MAVLink 2 không ký số; dialect `common` |
| Lõi xử lý MAVLink | B dùng thư viện MAVLink C v2 trong một adapter thực hiện `IParser` và `IDecoder` |
| Phụ thuộc thư viện | Khi thêm thư viện, B khóa commit thực tế và ghi SHA vào metadata; không thay thư viện giữa các lượt đo |
| Trường hợp môn học yêu cầu tự viết parser | B thay lõi adapter và giữ nguyên giao diện; thư viện tham chiếu vẫn dùng để tạo/đối chiếu mẫu chuẩn |
| Dữ liệu TCP | Các khung MAVLink gốc nối tiếp; giữ nguyên từng byte; không thêm JSON, độ dài riêng hay `frame_id` vào luồng |
| Đơn vị thời gian nội bộ | Số nguyên nanosecond từ mốc `steady_clock` chung của từng tiến trình |
| Vai trò điều khiển | Xác định từ cổng TCP đã accept; gateway quản lý, không tin vai trò do client tự khai |
| Giao diện người dùng | Console; client nhận raw rồi dùng lại parser/decoder của B |

Thư viện MAVLink C hỗ trợ cả v1 và v2; adapter của B vẫn phải áp dụng phạm vi chỉ nhận v2 không ký đã chốt cho sản phẩm. Không dùng trạng thái parser toàn cục cho tất cả kết nối. Trạng thái được cô lập theo nguồn/session; nếu dùng hàm thư viện có trạng thái channel thì phải bảo đảm mỗi nguồn có trạng thái độc lập.

## 2. Cổng và nhận diện PX4

| Đầu nhận | Bind mặc định | Bên kết nối/gửi |
|---|---|---|
| Gateway nhận telemetry | UDP `0.0.0.0:14550` | PX4 SITL hoặc bộ phát lại của C |
| Gateway nhận client quan sát | TCP `0.0.0.0:5760` | Tối đa 9 observer |
| Gateway nhận client điều khiển | TCP `0.0.0.0:5761` | Tối đa 1 controller |

Client chạy cùng máy gateway dùng `gateway_host=127.0.0.1`. Client máy khác dùng địa chỉ IPv4 thực tế của máy gateway. Với PX4 trên WSL/Linux, C cấu hình đích UDP là địa chỉ gateway mà môi trường đó truy cập được, cổng 14550; địa chỉ thực tế và chế độ mạng phải được ghi vào `run.json`.

Chính sách khóa PX4:

1. Khởi động ở trạng thái chưa xác định endpoint; chưa chuyển bản tin uplink nào về PX4.
2. A nhận datagram và B kiểm tra khung. HEARTBEAT hợp lệ có `sysid=1`, `compid=1` theo cấu hình sẽ xác nhận nguồn PX4.
3. A lưu đúng IP và cổng nguồn của datagram đó làm đích UDP gửi về. Không mặc định cổng nguồn của PX4 cũng là 14550.
4. Sau khi khóa, chỉ nhận telemetry từ endpoint đã khóa; nguồn khác bị bỏ và có sự kiện `unexpected_source`. Trước khi khóa, chỉ một tập endpoint ứng viên có giới hạn được kiểm tra: tối đa 16; ứng viên mới vượt giới hạn bị bỏ và ghi sự kiện. Không tự cấp trạng thái UDP vô hạn.
5. Bản tin từ endpoint khác không được thay đích gửi lệnh. Đổi endpoint yêu cầu cấu hình lại/khởi động lượt chạy mới.
6. HEARTBEAT đúng nguồn quá 3 giây không cập nhật thì đánh dấu cũ và từ chối lệnh mới với `heartbeat_stale`. HEARTBEAT trở lại đúng endpoint khôi phục trạng thái sẵn sàng.

`sysid/compid` trên là giá trị khởi đầu. C xác nhận chúng trong SITL và sửa cấu hình nếu khác. Chế độ phát lại cũng phải phát HEARTBEAT đúng ID trước để được khóa nguồn.

## 3. Session và source được cấp như thế nào

| Định danh | Bên tạo | Quy tắc |
|---|---|---|
| `session_id` | A ở gateway; C ở client | Số nguyên 64 bit tăng dần trong tiến trình, bắt đầu từ 1; mỗi kết nối TCP mới nhận ID mới; UDP dùng 0 |
| `source_id` | Chủ sở hữu mạng của tiến trình | Số nguyên 64 bit tăng dần trên mọi nguồn vào: một endpoint UDP hoặc một session TCP; không tái sử dụng trong tiến trình |
| `frame_id` | A ở đường xử lý gateway; C cấp ID cục bộ ở client | Tăng dần cho từng khung hợp lệ đã tách, bắt đầu 1; không dùng SEQ làm ID; parser trả về 0 trước khi bên gọi gắn ID |
| `request_id` | Command Manager của C | ID cục bộ của một yêu cầu điều khiển, giữ nguyên qua các lần gửi lại; không gửi thêm vào MAVLink |
| `run_id` | Công cụ chạy thử của C | Cấp một lần cho lượt thử; truyền cho mọi tiến trình; ghi thời gian bắt đầu kèm múi giờ |

Danh tính thống kê SEQ là `(source_id, sysid, compid)`. Source ID bao gồm kênh/phiên nguồn, nên hai nguồn cùng sysid vẫn không dùng chung thống kê. SEQ về 0 riêng lẻ không tạo phiên mới. Khi có bằng chứng nguồn khởi động lại hoặc người chạy chủ động reset phiên, chủ mạng cấp source ID mới và dọn trạng thái cũ.

Khi TCP mất kết nối, A xóa session, đóng hàng đợi và gọi `parser.reset(source_id)` trên worker đang sở hữu parser sau khi worker đã ngừng nhận thêm dữ liệu cho nguồn đó. Kết nối lại không được dùng phần byte hay SEQ cũ. Mỗi parser instance thuộc một worker; các worker không gọi đồng thời cùng instance. Inspector tự bảo vệ việc cập nhật và đọc snapshot.

ID chỉ có ý nghĩa trong tiến trình tạo ra nó. Khóa một khung trong log là `(run_id, component, frame_id)`, không so sánh trực tiếp ID của gateway với ID của client.

## 4. Giao diện A đưa dữ liệu cho B

Đầu vào chính thức là `ReceiveEvent` trong header chung:

```cpp
struct ReceiveEvent {
    SourceRef source;       // source_id, session_id, UDP/TCP, chiều dữ liệu, peer
    MonoNs received_at_ns;  // mốc nhận của sự kiện này
    Bytes bytes;           // sở hữu bản sao byte; không trỏ vào buffer recv tái sử dụng
};

ParseBatch IParser::consume(const ReceiveEvent& event);
```

A gọi `consume()` mỗi datagram UDP hoặc mỗi phần byte TCP nhận được, không cần tự tìm đầu MAVLink. Trường `peer.port` dùng host byte order. `Flow::TelemetryDownlink` là dữ liệu drone đến client; `Flow::ControlUplink` là dữ liệu từ controller về drone.

`ParseBatch` trả về hai tập độc lập:

- `frames`: chỉ chứa khung v2 không ký được chấp nhận, giữ thứ tự đầu vào, có `raw` nguyên bản và các trường header. `received_at_ns` là thời điểm sự kiện đã cung cấp đủ byte cuối cùng của khung.
- `issues`: các sự kiện `BadCrc`, `UnsupportedMessage`, `UnsupportedSigning`, `UnsupportedIncompatFlags`, `TruncatedDatagram`, `NoiseDiscarded`, `TcpBufferLimit`.

TCP thiếu byte là trạng thái chờ, chưa tính là khung lỗi; lần gọi có thể không trả khung hay issue. UDP không mang byte dở sang datagram kế tiếp. Một datagram có nhiều khung thì xuất mọi khung hoàn chỉnh và ghi riêng phần cuối bị cắt. Các trường hợp nhiễu trước khung được đếm theo byte; không coi mỗi byte nhiễu là một khung CRC lỗi.

B tính CRC cho các bản tin biết metadata trong `common`, không chỉ 5 bản tin cần giải mã. Khung ký hoặc incompat flag chưa hỗ trợ bị loại theo chính sách; compat flag không biết không tự làm khung bị loại. Chưa biết msgid được tính là chưa hỗ trợ. Với header đủ tin cậy, tiêu thụ đúng chiều dài của ứng viên chưa hỗ trợ; sau CRC sai, tái đồng bộ và chứng minh bằng bộ mẫu lỗi có đáp án chuẩn.

A gắn `frame_id` cho mỗi khung nhận từ B, sau đó tạo `shared_ptr<const FrameRecord>`. Các hàng đợi client cùng chia sẻ bản ghi bất biến đó; không sửa raw hay cấp lại ID theo từng client.

## 5. Decoder và Inspector của B

```cpp
std::optional<DecodedMessage> IDecoder::decode(const FrameRecord& frame) const;
void IInspector::on_frame(const FrameRecord& frame,
                         const std::optional<DecodedMessage>& decoded);
void IInspector::on_issue(const ParseIssue& issue);
InspectorSnapshot IInspector::snapshot() const;
```

| Loại dữ liệu đã giải mã | Trường giao tiếp giữa B và C |
|---|---|
| `Heartbeat` | `custom_mode`, `type`, `autopilot`, `base_mode`, `system_status` |
| `SysStatus` | `battery_voltage_v`, `battery_remaining_percent`; `nullopt` biểu diễn không xác định |
| `Attitude` | `roll_rad`, `pitch_rad`, `yaw_rad` |
| `GlobalPosition` | `latitude_deg`, `longitude_deg`, `altitude_msl_m`, `relative_altitude_m` |
| `CommandAck` | `command`, `result`, `progress`, `result_param2`, `target_system`, `target_component` |

B đối chiếu metadata chính thức để chuyển đúng đơn vị và giữ giá trị đặc biệt. Payload rút gọn được bổ sung byte 0 vào vùng giải mã đã khởi tạo; raw vẫn giữ nguyên. Bản tin hợp lệ ngoài 5 loại trên trả `nullopt` khi decode, nhưng vẫn được chuyển tiếp và tính SEQ. Các extension ACK được giải mã theo quy tắc zero-fill; giá trị đích 0 không phải một đích cụ thể để loại ACK.

Trong B1/B2, Inspector thống kê trên toàn bộ khung hợp lệ của mỗi nguồn trước khi lọc hiển thị:

- Khung đầu tiên: khởi tạo SEQ tham chiếu, không suy ra thiếu trước đó.
- `d=(seq_moi-seq_moc+256)%256` bằng 1: tăng received và cập nhật mốc.
- d từ 2 đến 127: tăng received, cộng d-1 vào thiếu ước lượng, cập nhật mốc.
- d bằng 0: tăng received và repeated; không tăng thiếu, giữ mốc.
- d từ 128 đến 255: tăng received và ambiguous; giữ mốc, không tăng thiếu. Lượt có ambiguous không dùng để khẳng định tỷ lệ mất gói chính xác.

`received_valid` tính các khung hợp lệ quan sát được, kể cả SEQ lặp; tỷ lệ thiếu chỉ dùng ở thí nghiệm sạch không trùng/đảo thứ tự. Các ứng viên lỗi CRC, chưa hỗ trợ, datagram bị cắt và byte nhiễu là các bộ đếm khác nhau; không cộng chúng với SEQ gap để suy ra tổng gói mất thật.

## 6. Luồng xử lý và các giới hạn

```text
A recv UDP -> hàng đợi nhận -> A gọi parser B -> khung hợp lệ
-> A gắn frame_id -> B giải mã/thống kê ở B1/B2
-> A đưa SharedFrame vào hàng đợi riêng từng client -> worker TCP gửi đủ raw

C gửi khung TCP -> A xác nhận session controller -> parser B kiểm tra khung
-> A kiểm tra bản tin/lệnh được phép và đích -> hàng đợi UDP -> PX4

PX4 trả ACK -> luồng telemetry phía trên -> C decoder -> Command Manager
```

| Tài nguyên | Giới hạn khởi đầu | Hành vi tại giới hạn |
|---|---|---|
| Hàng đợi nhận UDP | 4.096 datagram | Bỏ datagram mới; tăng `udp_queue_drop` |
| Bộ đệm parser TCP | 65.536 byte/session | Tách các khung trước khi kiểm tra phần còn dư; nếu vẫn vượt thì issue `TcpBufferLimit`, trả `close_tcp_session=true`, A đóng session |
| Hàng đợi gửi mỗi client | 1.024 khung và 1.048.576 byte | Nếu thêm khung sẽ vượt một trong hai giới hạn, bỏ khung mới và đóng riêng client đó; log `client_queue_full`; phần còn tồn ghi `connection_closed` |
| Hàng đợi gửi UDP uplink | 256 khung | Bỏ khung mới; ghi `udp_uplink_queue_full`; không tạo ACK MAVLink giả; Command Manager quan sát timeout |
| Hàng đợi Logger | 10.000 bản ghi | Bỏ bản ghi mới; tăng `log_drop`, đưa số đếm ra snapshot/summary |
| Tổng client | 9 observer và 1 controller | Từ chối kết nối vượt vai trò tương ứng |

Một worker sở hữu việc gửi trên mỗi socket. Với TCP, worker giữ offset và tiếp tục gửi nếu lần send trước chỉ nhận một phần byte. Client của C cũng có đúng một worker/hàng đợi gửi cho cả HEARTBEAT và COMMAND_LONG để không xen kẽ byte từ hai thread.

Chỉ giữ khóa khi cập nhật trạng thái hoặc lấy snapshot/danh sách. Không giữ khóa chung trong lúc send, ghi file hay join worker. Khi shutdown, đóng hàng đợi, đánh thức worker, shutdown socket và giải phóng mỗi socket một lần. Hết thời gian drain thì ghi số khung còn tồn trước khi hủy.

## 7. Vai trò và giao dịch lệnh

A từ chối dữ liệu uplink từ observer, đóng session vi phạm và ghi `observer_uplink_denied`. Controller thứ hai bị từ chối ngay khi accept; khi controller cũ đã đóng, controller mới có thể kết nối với session ID mới.

Uplink được chấp nhận gồm HEARTBEAT GCS có ID GCS cấu hình và COMMAND_LONG của cùng GCS với command 400 (ARM), 22 (TAKEOFF), 21 (LAND). COMMAND_LONG phải có target system/component đúng PX4 đã xác nhận; không dùng đích broadcast trong phạm vi demo. Các bản tin/lệnh khác bị loại và log `uplink_not_allowed` hoặc `target_mismatch`.

Trước khi xác nhận endpoint PX4 hoặc khi HEARTBEAT cũ, mọi uplink đều bị loại có log, không đoán đích. Gateway không tạo COMMAND_ACK thay PX4; client vẫn xử lý trường hợp không nhận ACK. Giá trị ID GCS mặc định là sysid 255, compid 190, cần tránh trùng với nguồn khác.

C dùng Command Manager với một yêu cầu đang chờ:

- Gửi lần đầu với confirmation=0; chờ ACK 1 giây.
- Nếu chưa có ACK, gửi lại tối đa 2 lần nữa, confirmation tăng theo lần gửi. Tổng cộng tối đa 3 lần gửi, không phải 3 lần gửi lại.
- ACK đang thực hiện: không gửi lại như chưa có ACK; chờ tối đa 10 giây không cập nhật, với giới hạn tổng 60 giây kể từ ACK đang thực hiện đầu tiên.
- Ghép ACK bằng command, sysid/compid của PX4 và đích ACK nếu có chỉ định. Bỏ/ghi ACK không khớp; theo dõi ACK đến muộn.
- ACK chấp nhận kết thúc bước xác nhận; xác nhận hành động bằng telemetry và trạng thái SITL. Không gọi TAKEOFF hoàn tất chỉ vì có ACK.
- Mất kết nối khi đang chờ: kết quả cục bộ `unknown`; không tự phát lại sau reconnect.

`request_id` không nằm trong MAVLink, nên ACK đến muộn của hai yêu cầu cùng command có thể nhập nhằng. Một yêu cầu đang chờ giảm rủi ro này nhưng không tạo ra mã giao dịch trên đường truyền; ghi giới hạn đó khi kiểm thử và không tuyên bố đối chiếu ACK tuyệt đối bằng request_id.

Đề cương đang chọn COMMAND_LONG cho ba lệnh. C phải kiểm chứng cách đóng gói, điều kiện và tham số TAKEOFF/LAND trên bản PX4 thực tế; nếu PX4 yêu cầu COMMAND_INT thì cập nhật danh sách uplink được phép cùng mẫu kiểm thử, không đổi ngầm chỉ một module. Tài liệu này chưa chốt một bộ tham số bay chưa được thử trên SITL.

## 8. Log và đo lường dùng chung

Mỗi tiến trình ghi vào `output/runs/<run_id>/<component>/`, với component là `gateway`, `observer_01`, ..., `controller`. CSV UTF-8, dấu phẩy, có header một lần; trường chứa dấu phẩy hoặc xuống dòng phải được quote; giá trị không có để ô trống, boolean là 0/1. Thời gian CSV là số nguyên ns tương đối với mốc steady_clock của chính tiến trình đó.

| Tệp | Header hoặc nội dung chốt |
|---|---|
| `run.json` | `schema_version`, `run_id`, `component`, `started_at_iso8601`, phần cứng, OS/mạng, commit chương trình, commit MAVLink, bản PX4, seed, cấu hình đã resolve, chính sách clock, endpoint PX4 sau khi khóa |
| `frames.csv` | `run_id,component,frame_id,source_id,session_id,rx_ns,sysid,compid,msgid,seq,payload_len,raw_len` |
| `issues.csv` | `run_id,component,source_id,session_id,event_ns,code,discarded_bytes,msgid` |
| `delivery.csv` | `run_id,component,frame_id,client_id,event_ns,send_done_ns,bytes_sent,status` |
| `commands.csv` | `run_id,component,request_id,session_id,command,target_system,target_component,attempt,event_ns,event,result,progress` |
| `events.csv` | `run_id,component,event_ns,session_id,source_id,event,detail` |
| `metrics.csv` | `run_id,component,sample_ns,valid_total,bad_crc_total,unsupported_total,truncated_datagrams,noise_bytes,seq_gap_total,seq_repeat_total,seq_ambiguous_total,udp_queue_drop,client_queue_drop,log_drop,active_clients,rx_frames_per_s,tx_bytes_per_s,cpu_percent,rss_bytes,udp_queue_depth,max_client_queue_frames` |
| `client_summary.csv` | `run_id,component,client_id,role,attempted_window,sent_window,not_sent_window,bytes_sent_window,latency_samples,latency_p95_ms,latency_max_ms,latency_sampling_truncated` |
| `summary.csv` | `run_id,component,mode,clients,measurement_s,valid_window,sent_window,not_sent_window,pending_after_drain,latency_samples,latency_p50_ms,latency_p95_ms,latency_max_ms,log_drop` |

`frames.csv` chỉ chứa khung chấp nhận; lỗi nằm trong `issues.csv`, không tạo frame ID giả cho byte nhiễu. Tên `ParseIssueCode` trong CSV dùng snake_case: `bad_crc`, `unsupported_message`, `unsupported_signing`, `unsupported_incompat_flags`, `truncated_datagram`, `noise_discarded`, `tcp_buffer_limit`. `DeliveryStatus` tương ứng: `sent_to_socket`, `client_queue_full`, `connection_closed`, `send_error`, `shutdown_discard`.

Các cột metrics kết thúc `_total` là tích lũy từ khi tiến trình bắt đầu; các cột rate/depth là mẫu của khoảng lấy mẫu. Các bộ đếm drop không có `_total` trong header vẫn là bộ đếm tích lũy, không phải giá trị riêng của 1 giây. `summary` chỉ lấy cửa sổ đo, bằng chênh bộ đếm/mốc thời gian sau warmup. CPU percent tính 0-100% của toàn bộ năng lực CPU logic, RSS dùng byte; không trộn với quy ước một lõi bằng 100%.

Độ trễ gateway theo từng client = `send_done_ns - rx_ns` của cùng khung, cùng tiến trình gateway. `send_done_ns` chỉ ghi khi đã gửi đủ raw vào socket; điều này không chứng minh client đã nhận. Khung chưa gửi đủ có `send_done_ns` trống. p95 dùng nearest-rank: sắp tăng n mẫu thành công, lấy vị trí `ceil(0.95*n)` tính từ 1; không có mẫu thì để trống. Báo số mẫu, khung không gửi và tồn sau drain bên cạnh độ trễ.

Không trừ clock giữa hai tiến trình hoặc lấy `time_boot_ms` của PX4 trừ clock Windows. Client không nhận `frame_id` của gateway trên TCP; thí nghiệm đối chiếu dùng manifest và fingerprint raw của tập chuẩn. Mẫu lặp raw giống hệt phải đối chiếu cả thứ tự/số lần xuất hiện; không dùng duy nhất hash làm ID duy nhất.

## 9. Ba chế độ và một lượt chạy cơ sở

| Chế độ | Bật | Log |
|---|---|---|
| B0 | Mạng, parser, CRC, định tuyến, bộ đếm nền và đo tối thiểu | `run.json`, `metrics.csv`, `summary.csv`, sự kiện hệ thống và lệnh; không log chi tiết theo khung/ứng viên |
| B1 | B0 + decoder và Inspector/SEQ | Giống B0, bổ sung số liệu Inspector trong metrics |
| B2 | B1 + log chi tiết | Thêm `frames.csv`, `issues.csv`, `delivery.csv` |

B0 vẫn đọc lượng tối thiểu của HEARTBEAT/COMMAND_LONG cần cho khóa nguồn và kiểm tra uplink; controller vẫn xử lý COMMAND_ACK cho giao thức lệnh. B0 không chạy toàn bộ decoder/Inspector. Các cột Inspector chưa đo trong B0 để trống, không ghi 0 gây hiểu nhầm. Tính latency summary bằng cùng bộ histogram/thu thập mẫu tối thiểu trong mọi chế độ, không phụ thuộc `delivery.csv` của B2; giữ giới hạn bộ nhớ theo tải/thời gian lượt chạy và ghi metadata nếu giảm mẫu. Các chế độ dùng cùng cơ chế đo.

Lượt cơ sở: 5 client tổng cộng (4 observer và 1 controller), 200 khung/giây từ bộ phát lại, warmup 10 giây, đo 60 giây, lấy snapshot 1 giây, drain tối đa 5 giây, seed 20261111. Đây là giá trị đầu vào mong muốn; C phải đo lại tốc độ phát thực tế. Chạy 3 lượt cho mỗi B0/B1/B2, luân phiên thứ tự chế độ, giữ cùng manifest và điều kiện máy.

Logger chỉ có hàng đợi 10.000 record, nhưng tệp log cũng phải có giới hạn: tối đa 100 MiB/tệp và 512 MiB/component/run. Chạm giới hạn thì ngừng ghi log chi tiết của component, tăng `log_drop` và ghi cờ `log_truncated` trong summary/run metadata; giữ khả năng ghi metadata/summary cuối lượt. Không dùng lượt bị cắt log để khẳng định đối chiếu đầy đủ từng khung.

## 10. Phần việc có thể bắt đầu ngay

| Người | Làm trước | Dữ liệu dùng khi chưa tích hợp | Bàn giao để người khác dùng |
|---|---|---|---|
| A | Socket UDP/TCP, session/source, hàng đợi, worker, send/shutdown, kiểm tra vai trò và đích | Đọc raw từ fixture; adapter giả lập trả sẵn khung hợp lệ; không coi adapter giả là parser hoàn chỉnh | Phần mạng đưa đúng ReceiveEvent; chuyển SharedFrame đúng byte; log thời gian gửi và lỗi mạng |
| B | Adapter IParser/IDecoder, Inspector, Logger/schema; bộ mẫu chuẩn | Tệp nhị phân và manifest sinh/đối chiếu bằng thư viện tham chiếu | Module xử lý không phụ thuộc Winsock; fixtures; kết quả U01-U11; API đúng header chung |
| C | SITL, client console, Command Manager, bộ phát lại, ACK giả lập và điều phối lượt đo | Server TCP giả phát fixture của B; dùng ACK giả để thử timeout | Client dùng lại module B; SITL/config mạng; công cụ thử và hướng dẫn chạy; metadata thật |

B và C cùng kiểm tra fixture lệnh, A kiểm tra việc cô lập session và client chậm. A cung cấp timestamp/giao nhận, B cung cấp xử lý/log/thống kê, C cung cấp bộ phát và script tổng hợp. C tổng hợp báo cáo; mỗi người tự viết và lưu bằng chứng cho module của mình.

## 11. Bộ fixture đầu tiên

B bàn giao dưới thư mục `tests/fixtures/v1/`. Bộ đầu tiên đã được sinh và đối chiếu độc lập; xem `docs/part-b.md` để chạy module và kiểm thử:

- `valid.bin`: nối 7 khung: 5 bản tin giải mã, COMMAND_LONG và PING hợp lệ.
- `manifest.json`: phiên bản/commit tham chiếu, seed, byte offset/độ dài mỗi khung, raw hex, header và trường giải mã mong đợi. SEQ và ID nguồn cố định, mô tả rõ đơn vị.
- `tcp_chunks.json`: các cách chia valid.bin, gồm một byte/lần và mọi điểm chia của mỗi khung. Đây là cách chia TCP, không phải ranh giới UDP.
- `udp_datagrams.json`: danh sách raw hex riêng cho từng datagram, có nhiều khung và một trường hợp cuối bị cắt.
- `errors.json`: biến thể sai CRC, nhiễu, unknown msgid, chữ ký, incompat flags không hỗ trợ và kết quả phân loại mong đợi.
- `seq_cases.json`: chuỗi 254-255-0-1, SEQ lặp, bước nhảy 11-14, gói cũ 20-19 và hai nguồn xen kẽ; ghi rõ gap/repeat/ambiguous/mốc cuối kỳ vọng.

Nếu dùng thư viện làm lõi parser, B phải thêm đáp án bằng tay hoặc vector cố định đã kiểm chứng cho header, thứ tự/độ dài, trường giải mã và SEQ. Không chỉ kiểm tra một lời gọi thư viện bằng chính lời gọi đó rồi kết luận module đã đúng.

## 12. Điều kiện nối các module

1. Header chung biên dịch độc lập ở C++17; cấu hình JSON đọc được. Đây chỉ là kiểm tra hợp đồng, chưa phải kiểm chứng chương trình.
2. B chạy fixture không cần socket; đầu ra khớp manifest, không trộn hai nguồn, không trả khung lỗi.
3. A nhận bộ phát lại UDP và cho 2 observer nhận cùng dãy raw; không thay byte hay thêm header ứng dụng.
4. C nối controller, HEARTBEAT GCS đi về đúng endpoint PX4; observer gửi uplink bị từ chối.
5. Thử ARM rồi TAKEOFF/LAND trên đúng bản SITL; lưu ACK, tham số và trạng thái thật. ACK giả chỉ chứng minh kiểm thử giao thức.
6. Sau khi các bước trên đạt, chạy B0/B1/B2 và ca client chậm. Mỗi người nộp log/bằng chứng, không thay số đo bằng số minh họa.

## 13. Nguồn đối chiếu

- Đề cương nội bộ: `docs/references/De_cuong_MAVLink_Gateway_PX4.docx`, đặc biệt mục 3-15.
- [MAVLink Packet Serialization](https://mavlink.io/en/guide/serialization.html): khung v2, CRC, flags và payload.
- [Using C MAVLink Libraries](https://mavlink.io/en/mavgen_c/): thư viện tham chiếu và cách dùng dialect.
- [MAVLink Command Protocol](https://mavlink.io/en/services/command.html): đóng gói lệnh, ACK và giới hạn cách đối chiếu.

Các lựa chọn như số cổng client, dung lượng hàng đợi, timeout, số lượt đo và chính sách khóa endpoint là quyết định của dự án, không phải giá trị bắt buộc của tiêu chuẩn MAVLink.

## 14. Bản tích hợp A+B

- Chỉ dùng contracts.hpp ở include/mavlink_gateway của thư mục gốc; bản trùng trong phần A đã xóa.
- Chỉ nhận JSON chung lồng nhóm; cấu hình phẳng cũ bị từ chối.
- run_id/stop_file/run_seconds và các tùy chọn ép send là CLI của gateway, không thêm khóa phẳng vào JSON chung.
- Gateway dùng module Parser/Decoder/Inspector/CsvLogger thật của B cho B0/B1/B2.
- Xem README.md và docs/running.md để build/chạy; scripts/test.ps1 gọi CTest để kiểm chứng.
