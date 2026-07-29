// outbound_smoke — 出站 SMTP FSM/Session/Types 功能完整性验证
#include "mail_system/back/outbound/outbound_smtp_fsm.h"
#include "mail_system/back/outbound/outbound_smtp_session.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/outbound/outbound_server.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

using namespace mail_system;
using namespace mail_system::outbound;

static int g_fail = 0;

static void check(const char* name, bool cond) {
    if (!cond) { std::cerr << "  FAIL: " << name << std::endl; ++g_fail; }
}

#define SECTION(name) std::cout << "  " << name << " ..." << std::endl

// ========== FSM 构造测试 ==========

static void test_fsm_construction() {
    SECTION("FSM construction and types");

    auto fsm = std::make_shared<OutboundSmtpFsm<TcpConnection>>();
    check("fsm created", fsm != nullptr);
}

// ========== MailDeliveryTask 类型验证 ==========

static void test_delivery_task_types() {
    SECTION("MailDeliveryTask types");

    auto task = std::make_unique<MailDeliveryTask>();
    task->mail_id = 1;
    task->record_id = 100;
    task->sender = "alice@example.com";
    task->recipient = "bob@example.com";
    task->attempt_count = 0;
    task->max_attempts = 8;

    check("mail_id", task->mail_id == 1);
    check("record_id", task->record_id == 100);
    check("sender", task->sender == "alice@example.com");
    check("recipient", task->recipient == "bob@example.com");
    check("attempt_count", task->attempt_count == 0);
    check("max_attempts", task->max_attempts == 8);
    check("mail_ptr null initially", task->mail_ptr == nullptr);
}

// ========== OutboundSmtpEvent enum 完整性 ==========

static void test_event_enum_values() {
    SECTION("Event enum values");

    check("CONNECT", static_cast<int>(OutboundSmtpEvent::CONNECT) >= 0);
    check("CONNECTED", static_cast<int>(OutboundSmtpEvent::CONNECTED) >= 0);
    check("GREETING_220", static_cast<int>(OutboundSmtpEvent::GREETING_220) >= 0);
    check("EHLO_250", static_cast<int>(OutboundSmtpEvent::EHLO_250) >= 0);
    check("MAIL_250", static_cast<int>(OutboundSmtpEvent::MAIL_250) >= 0);
    check("RCPT_250", static_cast<int>(OutboundSmtpEvent::RCPT_250) >= 0);
    check("DATA_354", static_cast<int>(OutboundSmtpEvent::DATA_354) >= 0);
    check("ACCEPT_250", static_cast<int>(OutboundSmtpEvent::ACCEPT_250) >= 0);
    check("QUIT_221", static_cast<int>(OutboundSmtpEvent::QUIT_221) >= 0);
    check("ERROR_4XX", static_cast<int>(OutboundSmtpEvent::ERROR_4XX) >= 0);
    check("ERROR_5XX", static_cast<int>(OutboundSmtpEvent::ERROR_5XX) >= 0);
    check("CONNECTION_LOST", static_cast<int>(OutboundSmtpEvent::CONNECTION_LOST) >= 0);
}

// ========== OutboundSmtpState enum 完整性 ==========

static void test_state_enum_values() {
    SECTION("State enum values");

    check("INIT", static_cast<int>(OutboundSmtpState::INIT) >= 0);
    check("CONNECTING", static_cast<int>(OutboundSmtpState::CONNECTING) >= 0);
    check("CONNECTED", static_cast<int>(OutboundSmtpState::CONNECTED) >= 0);
    check("EHLO", static_cast<int>(OutboundSmtpState::EHLO) >= 0);
    check("MAIL_FROM", static_cast<int>(OutboundSmtpState::MAIL_FROM) >= 0);
    check("RCPT_TO", static_cast<int>(OutboundSmtpState::RCPT_TO) >= 0);
    check("DATA", static_cast<int>(OutboundSmtpState::DATA) >= 0);
    check("DATA_BODY", static_cast<int>(OutboundSmtpState::DATA_BODY) >= 0);
    check("WAIT_ACCEPT", static_cast<int>(OutboundSmtpState::WAIT_ACCEPT) >= 0);
    check("CLOSED", static_cast<int>(OutboundSmtpState::CLOSED) >= 0);
}

int main() {
    std::cout << "Outbound Smoke Test Suite\n========================\n";

    test_fsm_construction();
    test_delivery_task_types();
    test_event_enum_values();
    test_state_enum_values();

    if (g_fail == 0) {
        std::cout << "\nAll outbound smoke tests passed.\n";
        return 0;
    }
    std::cerr << '\n' << g_fail << " test(s) FAILED.\n";
    return 1;
}
