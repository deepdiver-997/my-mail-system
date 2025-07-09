#include "include/mail_system/back/mailServer/fsm/TraditionalSmtpsFsm.h"
#include "include/mail_system/back/mailServer/fsm/BoostMsmSmtpsFsm.h"
#include <iostream>
#include <memory>
#include <cassert>

using namespace mail_system;
using namespace mail_system::SMTPS;

// 测试辅助函数
void printResult(const std::string& testName, const std::pair<int, std::string>& result) {
    std::cout << testName << ": " << result.first << " " << result.second << std::endl;
}

// 测试状态机基本功能
void testFsmBasics(SmtpsFsm& fsm) {
    SmtpsContext context;
    context.state = SmtpsState::INIT;
    
    // 测试EHLO命令
    auto result = fsm.processEvent(SmtpsEvent::EHLO, "client.example.com", context);
    printResult("EHLO", result);
    assert(context.state == SmtpsState::EHLO);
    
    // 测试MAIL FROM命令
    result = fsm.processEvent(SmtpsEvent::MAIL, "FROM: <sender@example.com>", context);
    printResult("MAIL FROM", result);
    assert(context.state == SmtpsState::MAIL_FROM);
    assert(context.sender == "sender@example.com");
    
    // 测试RCPT TO命令
    result = fsm.processEvent(SmtpsEvent::RCPT, "TO: <recipient@example.com>", context);
    printResult("RCPT TO", result);
    assert(context.state == SmtpsState::RCPT_TO);
    assert(context.recipients.size() == 1);
    assert(context.recipients[0] == "recipient@example.com");
    
    // 测试DATA命令
    result = fsm.processEvent(SmtpsEvent::DATA, "", context);
    printResult("DATA", result);
    assert(context.state == SmtpsState::DATA);
    
    // 测试DATA内容
    result = fsm.processEvent(SmtpsEvent::DATA_LINE, "Subject: Test Email", context);
    assert(result.first == 0); // 不需要响应
    
    result = fsm.processEvent(SmtpsEvent::DATA_LINE, "", context);
    assert(result.first == 0);
    
    result = fsm.processEvent(SmtpsEvent::DATA_LINE, "This is a test email.", context);
    assert(result.first == 0);
    
    // 测试结束DATA
    result = fsm.processEvent(SmtpsEvent::DATA_LINE, ".", context);
    printResult("DATA END", result);
    assert(context.state == SmtpsState::EHLO);
    
    // 测试QUIT命令
    result = fsm.processEvent(SmtpsEvent::QUIT, "", context);
    printResult("QUIT", result);
}

int main() {
    std::cout << "Testing Traditional SMTP FSM..." << std::endl;
    TraditionalSmtpsFsm traditionalFsm;
    traditionalFsm.initialize(nullptr); // 在测试中我们不需要实际的数据库连接
    testFsmBasics(traditionalFsm);
    
    std::cout << "\nTesting Boost.MSM SMTP FSM..." << std::endl;
    BoostMsmSmtpsFsm boostFsm;
    boostFsm.initialize(nullptr);
    testFsmBasics(boostFsm);
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}