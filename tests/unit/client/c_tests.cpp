// Test Command Manager bang dong ho gia (khong sleep, khong socket, khong MAVLink).
#include "command_manager.hpp"
#include <cstdio>
#include <memory>
#include <vector>

using namespace cclient;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; std::printf("  FAIL dong %d: %s\n", __LINE__, #cond); } } while (0)

constexpr MonoNs MS = 1000000ULL;
constexpr MonoNs S = 1000 * MS;

struct Harness {
    std::vector<std::uint8_t> confirmations; // confirmation cua tung lan gui
    std::vector<CommandEvent> events;
    bool send_ok = true;
    std::unique_ptr<CommandManager> mgr;
    Harness() {
        mgr = std::make_unique<CommandManager>(
            CommandConfig{},
            [this](std::uint16_t cmd, const Params&, std::uint8_t conf) {
                confirmations.push_back(conf);
                return Bytes{static_cast<std::uint8_t>(cmd & 0xff), conf};
            },
            [this](const Bytes&) { return send_ok; },
            [this](const CommandEvent& e) { events.push_back(e); });
    }
    bool has_event(const char* name) const {
        for (const auto& e : events) if (e.event == name) return true;
        return false;
    }
    static CommandAck ack(std::uint16_t cmd, std::uint8_t result, std::uint8_t target = 255, std::uint8_t progress = 0) {
        CommandAck a;
        a.command = cmd; a.result = result; a.progress = progress; a.target_system = target;
        return a;
    }
};

static void test_accept() {
    std::puts("[1] ACK accepted ket thuc yeu cau");
    Harness h;
    CHECK(h.mgr->submit(kCmdArm, {}, 0) == 1);
    CHECK(h.mgr->busy());
    h.mgr->on_ack(Harness::ack(kCmdArm, 0), 1, 1, 100 * MS);
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Accepted);
    CHECK(h.confirmations.size() == 1 && h.confirmations[0] == 0);
}
static void test_retry_then_timeout() {
    std::puts("[2] khong co ACK: tong 3 lan gui, confirmation 0,1,2, roi Timeout");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    h.mgr->tick(999 * MS);   CHECK(h.confirmations.size() == 1);
    h.mgr->tick(1 * S);      CHECK(h.confirmations.size() == 2);
    h.mgr->tick(2 * S);      CHECK(h.confirmations.size() == 3);
    CHECK(h.mgr->busy());
    h.mgr->tick(3 * S);
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
    CHECK(h.confirmations.size() == 3);
    CHECK(h.confirmations[0] == 0 && h.confirmations[1] == 1 && h.confirmations[2] == 2);
    h.mgr->tick(10 * S);
    CHECK(h.confirmations.size() == 3);
}
static void test_busy() {
    std::puts("[3] chi 1 yeu cau dang cho");
    Harness h;
    CHECK(h.mgr->submit(kCmdArm, {}, 0) == 1);
    CHECK(h.mgr->submit(kCmdLand, {}, 10 * MS) == 0);
    CHECK(h.has_event("rejected_busy"));
    CHECK(h.confirmations.size() == 1);
}
static void test_ack_filtering() {
    std::puts("[4] ACK sai nguon / sai command / sai dich bi bo qua; target_system=0 van nhan");
    Harness h;
    h.mgr->submit(kCmdArm, {}, 0);
    h.mgr->on_ack(Harness::ack(kCmdArm, 0), 2, 1, 10 * MS);        // sai sysid
    h.mgr->on_ack(Harness::ack(kCmdArm, 0), 1, 2, 10 * MS);        // sai compid
    h.mgr->on_ack(Harness::ack(kCmdLand, 0), 1, 1, 10 * MS);       // sai command
    h.mgr->on_ack(Harness::ack(kCmdArm, 0, 77), 1, 1, 10 * MS);    // gui cho GCS khac
    CHECK(h.mgr->busy());
    h.mgr->on_ack(Harness::ack(kCmdArm, 0, 0), 1, 1, 20 * MS);     // dich 0: chap nhan
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Accepted);
}
static void test_rejected() {
    std::puts("[5] ACK DENIED/FAILED -> Rejected, khong gui lai");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 2), 1, 1, 50 * MS);
    CHECK(h.mgr->last_outcome() == Outcome::Rejected);
    h.mgr->tick(5 * S);
    CHECK(h.confirmations.size() == 1);
}
static void test_in_progress_then_accept() {
    std::puts("[6] IN_PROGRESS roi ACCEPTED: khong gui lai trong luc cho");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 5, 255, 30), 1, 1, 500 * MS);
    h.mgr->tick(5 * S);
    CHECK(h.mgr->busy());
    CHECK(h.confirmations.size() == 1);
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 0), 1, 1, 6 * S);
    CHECK(h.mgr->last_outcome() == Outcome::Accepted);
}
static void test_in_progress_idle_timeout() {
    std::puts("[7] IN_PROGRESS khong cap nhat 10 s -> Timeout");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 5), 1, 1, 500 * MS);
    h.mgr->tick(10 * S + 499 * MS); CHECK(h.mgr->busy());
    h.mgr->tick(10 * S + 500 * MS); CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
    CHECK(h.confirmations.size() == 1);
}
static void test_in_progress_total_timeout() {
    std::puts("[8] IN_PROGRESS cap nhat lien tuc van bi cat o 60 s tinh tu ACK dau");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    MonoNs t = 500 * MS;
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 5), 1, 1, t);
    for (t = 5500 * MS; t < 60 * S; t += 5 * S) {
        h.mgr->on_ack(Harness::ack(kCmdTakeoff, 5), 1, 1, t);
        h.mgr->tick(t);
        CHECK(h.mgr->busy());
    }
    h.mgr->on_ack(Harness::ack(kCmdTakeoff, 5), 1, 1, 60 * S);
    h.mgr->tick(60 * S + 500 * MS);
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
}
static void test_disconnect() {
    std::puts("[9] mat ket noi khi dang cho -> Unknown, khong tu gui lai, khong nhan lenh moi");
    Harness h;
    h.mgr->submit(kCmdArm, {}, 0);
    h.mgr->on_disconnect(300 * MS);
    CHECK(h.mgr->last_outcome() == Outcome::Unknown);
    h.mgr->tick(5 * S);
    CHECK(h.confirmations.size() == 1);
    CHECK(h.mgr->submit(kCmdLand, {}, 6 * S) == 0);
    CHECK(h.has_event("rejected_disconnected"));
}
static void test_late_ack() {
    std::puts("[10] ACK den muon sau Timeout: ghi ack_late, ket qua khong doi");
    Harness h;
    h.mgr->submit(kCmdArm, {}, 0);
    h.mgr->tick(1 * S); h.mgr->tick(2 * S); h.mgr->tick(3 * S);
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
    h.mgr->on_ack(Harness::ack(kCmdArm, 0), 1, 1, 4 * S);
    CHECK(h.has_event("ack_late"));
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
}
static void test_send_failure() {
    std::puts("[11] gui loi -> SendFailed, giai phong yeu cau");
    Harness h;
    h.send_ok = false;
    CHECK(h.mgr->submit(kCmdArm, {}, 0) == 0);
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::SendFailed);
}
static void test_request_ids() {
    std::puts("[12] request_id tang dan, giu nguyen qua cac lan gui lai");
    Harness h;
    CHECK(h.mgr->submit(kCmdArm, {}, 0) == 1);
    h.mgr->tick(1 * S);
    h.mgr->on_ack(Harness::ack(kCmdArm, 0), 1, 1, 1100 * MS);
    CHECK(h.mgr->submit(kCmdLand, {}, 2 * S) == 2);
    int with_id1 = 0;
    for (const auto& e : h.events) if (e.request_id == 1 && e.event == "queued") ++with_id1;
    CHECK(with_id1 == 2);
}

static void test_ack_target_combinations() {
    std::puts("[13] ACK target system/component: dich 0 duoc bo trong, dich cu the phai khop");
    const struct { std::uint8_t system, component; bool accepted; } cases[] = {
        {255, 190, true}, {255, 0, true}, {0, 190, true}, {0, 0, true},
        {77, 190, false}, {77, 0, false}, {77, 189, false},
        {255, 189, false}, {0, 189, false},
    };
    for (const auto& test : cases) {
        Harness h;
        h.mgr->submit(kCmdArm, {}, 0);
        auto ack = Harness::ack(kCmdArm, 0, test.system);
        ack.target_component = test.component;
        h.mgr->on_ack(ack, 1, 1, 100 * MS);
        CHECK(h.mgr->busy() == !test.accepted);
        CHECK(h.mgr->last_outcome() == (test.accepted ? Outcome::Accepted : Outcome::None));
        CHECK(h.has_event(test.accepted ? "ack_accepted" : "ack_ignored"));
        CHECK(h.confirmations.size() == 1);
    }
}

static void test_late_ack_target_component() {
    std::puts("[14] ACK muon sai component phai la ignored, khong phai late");
    Harness h;
    h.mgr->submit(kCmdArm, {}, 0);
    h.mgr->tick(1 * S); h.mgr->tick(2 * S); h.mgr->tick(3 * S);
    auto ack = Harness::ack(kCmdArm, 0);
    ack.target_component = 189;
    h.mgr->on_ack(ack, 1, 1, 4 * S);
    CHECK(h.has_event("ack_ignored"));
    CHECK(!h.has_event("ack_late"));
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
    ack.target_component = 190;
    h.mgr->on_ack(ack, 1, 1, 5 * S);
    CHECK(h.has_event("ack_late"));
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
}

static void test_wrong_progress_target_component() {
    std::puts("[15] IN_PROGRESS sai component khong duoc keo dai timeout");
    Harness h;
    h.mgr->submit(kCmdTakeoff, {}, 0);
    auto ack = Harness::ack(kCmdTakeoff, 5);
    ack.target_component = 190;
    h.mgr->on_ack(ack, 1, 1, 500 * MS);
    ack.target_component = 189;
    h.mgr->on_ack(ack, 1, 1, 9 * S);
    CHECK(h.mgr->busy());
    CHECK(h.has_event("ack_ignored"));
    h.mgr->tick(10 * S + 500 * MS);
    CHECK(!h.mgr->busy());
    CHECK(h.mgr->last_outcome() == Outcome::Timeout);
    CHECK(h.confirmations.size() == 1);
}

int main() {
    test_accept(); test_retry_then_timeout(); test_busy(); test_ack_filtering(); test_rejected();
    test_in_progress_then_accept(); test_in_progress_idle_timeout(); test_in_progress_total_timeout();
    test_disconnect(); test_late_ack(); test_send_failure(); test_request_ids();
    test_ack_target_combinations(); test_late_ack_target_component(); test_wrong_progress_target_component();
    std::printf("\n%d/%d kiem tra dat, %d that bai\n", g_checks - g_fail, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
