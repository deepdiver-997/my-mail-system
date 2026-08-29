// bcrypt 标准兼容回归测试
//
// 剧情：src/.../bcrypt.cpp 曾被"round-trip 自洽"掩盖的几个 bug 弄坏 ——
//  1) b64_encode 把 16/23 字节累进 32 位 unsigned int 溢出，只留最后 4 字节；
//  2) 只对 magic 加密 1 个 64-bit 块（标准是 3 个块共 24 字节）；
//  3) S[3][211] 表错值 0x56cccd（标准 0x56cccd02）；
//  4) eks_blowfish_setup 与 OpenBSD 参考多处分歧（缺每轮 P-XOR、block 未归零、
//     轮内 salt/key 顺序颠倒等）。
// 后果：C++ 生成的 hash 不是标准 bcrypt（python/openssl 均不认），verify 对
// C++ 自产的退化 hash 恒 true。自产自验（round-trip）永远测不出来。
//
// 守卫：本测试用独立标准实现（python bcrypt）生成的已知向量——
// 正确密码必须通过、错误密码必须拒绝。这是旧实现唯一的盲区方向。
// 向量由 python bcrypt 生成（2026-08-29），固定 (密码,盐,成本)。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

#include "mail_system/back/common/bcrypt.h"

using namespace mail_system;

static int g_pass = 0, g_fail = 0;

static void expect_verify(const std::string& pw, const std::string& hash,
                          bool want, const std::string& what) {
    bool got = bcrypt_verify(pw, hash);
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        std::printf("  FAIL %s: verify(%s) got %d, want %d\n",
                    what.c_str(), pw.c_str(), (int)got, (int)want);
    }
}

int main() {
    // 独立标准实现(python bcrypt)生成的已知向量 —— 旧实现对标准 hash 一律拒绝
    struct Vec { const char* pw; const char* hash; };
    const Vec kVec[] = {
        {"correct-pass", "$2b$10$G6UKBKVUTqjxm2HjR5qp8OV8a.EZ99K4C1RIYv95wQDNad6evG1Yu"},
        {"tYgjmUh#Bel",  "$2b$08$ZzKyh2LZ45pD5aBA2brh0uISzsvWGVry0ZK71xB2KGB931V2zLtgK"},
        {"iEl2hpChYgCf", "$2b$12$zqtQ9yYUVIjPmApmLOZBW.nBWKt.kjM6CkO/n5rPO6w4ZSJMNBHCG"},
        {"L1spNxny",     "$2b$08$aYhgpqzYNYfublrrI1YNgOxuWI0R2qGo56U.4Djz6gRES8WoxkqB6"},
    };
    for (const Vec& v : kVec) {
        expect_verify(v.pw, v.hash, true,  std::string("known-vector correct ") + v.pw);
        expect_verify("wrong-password", v.hash, false, std::string("known-vector wrong ") + v.pw);
    }

    // round-trip：随机 salt hash 后必须能验证、错误密码必须拒绝
    {
        std::string h = bcrypt_hash("round-trip-pass", 4);
        if (h.size() == 60 && h.compare(0, 4, "$2b$") == 0) {
            g_pass++;
        } else {
            g_fail++;
            std::printf("  FAIL hash format: [%s] len %zu\n", h.c_str(), h.size());
        }
        expect_verify("round-trip-pass", h, true,  "roundtrip correct");
        expect_verify("wrong", h, false, "roundtrip wrong");
    }

    // 结构：同密码不同随机 salt 必须产出不同 hash
    {
        std::string h1 = bcrypt_hash("same-pw", 4);
        std::string h2 = bcrypt_hash("same-pw", 4);
        if (h1 != h2) {
            g_pass++;
        } else {
            g_fail++;
            std::printf("  FAIL same pw produced identical hash? %s\n", h1.c_str());
        }
    }

    std::printf("bcrypt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
