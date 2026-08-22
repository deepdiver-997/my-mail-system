// MIME 解析 fuzz harness（libFuzzer）。
//
// parse_mime_tree 是整个系统面对「任意互联网邮件」的第一解析入口：
// 递归 multipart、boundary 字符串、header 折叠、无 header 的裸正文，
// 都是历史 bug 的高发区（见 docs/bugfixes/）。纯函数、零 I/O，
// 是 ROI 最高的 fuzz 目标。
//
// 构建（macOS 需 Homebrew LLVM，Apple clang 无 libFuzzer 运行时）：
//   CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
//     cmake -B build-fuzz -DENABLE_FUZZING=ON -DBUILD_TESTS=OFF
//   cmake --build build-fuzz --target mime_fuzz -j
// 运行：./build-fuzz/mime_fuzz test/fuzz/corpus/mime \
//         -max_total_time=60 -rss_limit_mb=2560 -malloc_limit_mb=512 \
//         -artifact_prefix=./crash-
#include "mail_system/back/common/mime_parser.h"

using namespace mail_system;

#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // MimePart 定义在 mail.h 的 mail_system 作用域（parse_mime_tree 同层）
    MimePart root;
    parse_mime_tree(
        std::string_view(reinterpret_cast<const char*>(data), size), root);
    return 0;
}
