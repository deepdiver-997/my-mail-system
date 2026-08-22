// SMTP 接收路径 fuzz harness（libFuzzer）。
//
// process_message_data 是 SMTP DATA 阶段的解析核心：header 缓冲、multipart
// 边界识别、附件检测、大小记账 —— 与 MIME 解析同为面对任意互联网输入的
// 第一入口。（dot-stuffing 还原在 SmtpsSession::handle_read 里、依赖 session
// 基础设施，暂不在本 harness 覆盖。）
//
// 两个覆盖技巧：
//   1. 每次运行全新 SmtpsContext —— 解析器跨调用累积状态（header/multipart/
//      附件），复用 context 会把用例耦合成分块顺序。
//   2. 输入首字节用作分块点：同一段数据按不同切分喂两次，覆盖「TCP 分段
//      与命令边界不对齐」——半个 \r\n、被从中间劈开的 boundary。
//
// 构建与运行参数见 mime_fuzz.cpp 头部注释（同一套 ENABLE_FUZZING 选项）。
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"
#include "mail_system/back/common/logger.h"

#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    // LOG_* 宏要求 logger 已初始化；off 级别，零输出开销
    mail_system::Logger::get_instance().init(
        "smtp_data_fuzz.log", 0, 1, spdlog::level::off, false, false);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace mail_system;
    const std::string input(reinterpret_cast<const char*>(data), size);

    for (const bool streaming : {true, false}) {
        SmtpsContext ctx;              // 全新状态，杜绝跨用例耦合
        ctx.streaming_enabled = streaming;
        const size_t split = size ? static_cast<size_t>(data[0]) % size : 0;
        algorithm::process_message_data(ctx, input.substr(0, split));
        algorithm::process_message_data(ctx, input.substr(split));
    }
    return 0;
}
