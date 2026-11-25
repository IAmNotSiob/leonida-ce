// Standalone test for runtime/shims/bcpack_decode.cpp (BCPack, DirectStorage compression 14).
// No game data: the vectors are the docs/textures.md BC7 example and synthetic chunks (random lane symbols encoded
// by tests/test_bcpack.py's test encoder; expected output from tools/bcpack.py strict decode).
// Byte-exact comparison on real chunks is done by /root/bcpack-port/compare.py (reads the pools in place).
//
// Native:  clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -I runtime/shims \
//            runtime/test/bcpack_decode_test.cpp runtime/shims/bcpack_decode.cpp -o bcpack_decode_test
// Windows: built like the other runtime tests (see the integration note).
#include "bcpack_decode.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

struct Vec {
    int mode;
    size_t size;
    const char* raw;
    const char* expected;
};

const Vec kVectors[] = {
    // tools/bcpack.py docs/textures.md example (test_bc7_fixed_stream_strict)
    {7, 16, "101010101010101004040303030304040101010101010140000000000000024002027546281800000101df5f4b7c03410202024110101010040404031010101040041001030303044001080001010104000000080000000041020994e4db27a4410511fe6b020005", "40317204a56819be023456789abcdef0"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {1, 40, "121a1b1d101c1e1407050709050b09060b05713f023bc99d33db9123005c32e1010ef4ff60fffffb30ade3f721f390943501caff60ffff99d9f8181e1b1307070b06150101f44bec0095a824ff4fbb52ce933fffff891c80fe48ff8aee2bfbf8dcff910b18ffffffb3d706f1fb12ff70faff68feff5c1ccb50c3c05dffae", "86f4efb7aaa2fc3862bec464afaf10cc4ceca2118505c7e1a1651919b2a15f56c1f3bdbf911f9770"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {3, 80, "151419111a1c1e1f05050f0a0f0d0f1352f9030e0c1dcc017551ebb135ec5401360a1e0ffffffeff4ccc2caefffbf4fd8ccee4fbffff8bfff6c5f6131418140a090d0b17140f2d5b5a8958f4e10920f3164e4e9caefea2ff93ff94ff6e7cb9e09afaf87fff35ff20e20bb28010021d06a676ff4fa5cfff352bddb601fcb7ff652dc3e3622fde8488ff3bbefff94620d5e1071c1f68ffffffe70310ffe16cbc7a9cc4dcff2f5cffee7b56062af1ecfff1", "817b36787d1196b4aa8b185c9505fbfaacd9d7eea7490adc1a657103aba89d8ddb7ef8f8e3000a910bf843f1b5c5aaa2d9014a62d7490238c5f7a3ec50561afafad7b69cd845d515989c1cbb50701786"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {4, 64, "1d191e1413131a1307070c0a09090d090103042302019e08f689492cc1c3d3e6080dff138d0afef3fc219a52fabad6bd5dcaff73b966e1e67b1411161d0909080e0a01010b27598d27270510ffb2b5b7f9505e0de68af4f134b9343f47d8323d8b790cffece9b066ffc8967d1397ffb9134bcb64f6f7e2ff1845ffeff2e75051effb26ffe305fc7d73849bfcffb82c52205204b3de50ff9e", "cd8319603c36903c8f4414003a5a9500ce22789a28d1689fced5946f00f6b6a5cffb1a0475781d5584e0cbcb12179f004b769b57103903faba777765575bb3d9"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {5, 16, "101010101010101004040303030303030101010110101010000000000000000002027902ec1cd0880101585a90188c880202101010100303030310101010000000002030040418a82c80", "43ac16031275d17581ad088135c99068"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {6, 16, "10101010101010100404030303040303010101010104040400000001000000000202ea009f0c68c001013104120038f00202051010101010030303030310101010101001010304030000000001010104947cc4ac00000100dc58c8c1d3337130fb00f802", "727442a2fb405ee21ae25f4ea2095b93"},
    // synthetic: random lanes encoded by tests/test_bcpack.py, decoded by tools/bcpack.py (strict)
    {7, 16, "191e15161f121c140303040405040504020117160102050dee0056a6007049f11920e604ffdeff4086028356e74a826ba0b8ff7815b41a10e83505031a1a1f150a040504050411111700014001010404ffd0003f00228402ef40fffeff0661d26bf5d7047c0520a8ffe3ff07c3f3fb6308f0", "d8847a2000c0a34730e8483fa1a95942"},
};

std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        v.push_back(static_cast<uint8_t>(nib(s[0]) << 4 | nib(s[1])));
    }
    return v;
}

int g_fail = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            ++g_fail;                                   \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                   \
            std::printf("\n");                          \
        }                                               \
    } while (0)

// Decoding must either succeed or return false with dst untouched; never crash.
size_t robust(const std::vector<uint8_t>& raw, int mode, size_t size) {
    std::vector<uint8_t> dst(size, 0x5a);
    std::string why;
    const bool ok = bcpack::decode(raw.data(), raw.size(), mode, dst.data(), dst.size(), &why);
    if (!ok) {
        bool untouched = true;
        for (uint8_t b : dst) untouched &= b == 0x5a;
        CHECK(untouched, "dst written on failure (mode %d)", mode);
        CHECK(!why.empty(), "no reason on failure");
    }
    return ok ? 1 : 0;
}

}  // namespace

int main() {
    int n = 0;
    for (const Vec& v : kVectors) {
        const auto raw = unhex(v.raw);
        const auto exp = unhex(v.expected);
        CHECK(exp.size() == v.size, "vector %d size", n);
        std::vector<uint8_t> dst(v.size, 0xcc);
        std::string why;
        int used = -1;
        CHECK(bcpack::decode(raw.data(), raw.size(), v.mode, dst.data(), dst.size(), &why, &used), "vector %d mode %d: %s", n, v.mode, why.c_str());
        CHECK(dst == exp, "vector %d mode %d: output differs", n, v.mode);
        // auto-detect and padded input (pool chunks carry zero padding)
        auto padded = raw;
        padded.resize(raw.size() + 200, 0);
        std::fill(dst.begin(), dst.end(), 0xcc);
        CHECK(bcpack::decode(padded.data(), padded.size(), bcpack::kAuto, dst.data(), dst.size(), &why, &used), "vector %d auto: %s", n, why.c_str());
        CHECK(dst == exp && used == v.mode, "vector %d auto: output/mode differs (got mode %d)", n, used);
        // wrong size / truncation / trailing garbage / other modes / every single-bit flip
        robust(raw, v.mode, v.size + 16);
        robust(raw, v.mode, v.size == 16 ? 8 : v.size - 8);
        robust(raw, v.mode, 0);
        for (size_t k = 0; k < raw.size(); ++k) robust(std::vector<uint8_t>(raw.begin(), raw.begin() + k), v.mode, v.size);
        auto junk = raw;
        junk.push_back(1);
        CHECK(!robust(junk, v.mode, v.size), "vector %d: trailing non-zero byte accepted", n);
        for (int m = 0; m <= 9; ++m) robust(raw, m, v.size);
        for (size_t k = 0; k < raw.size(); ++k)
            for (int bit = 0; bit < 8; ++bit) {
                auto mut = raw;
                mut[k] ^= static_cast<uint8_t>(1u << bit);
                robust(mut, v.mode, v.size);
                robust(mut, bcpack::kAuto, v.size);
            }
        ++n;
    }
    // random garbage
    std::mt19937 rng(12345);
    for (int t = 0; t < 20000; ++t) {
        std::vector<uint8_t> raw(rng() % 600);
        for (auto& b : raw) b = static_cast<uint8_t>(rng());
        if (t & 1)  // plausible header bytes
            for (size_t k = 0; k < raw.size() && k < 16; ++k) raw[k] = static_cast<uint8_t>(0x10 | (rng() & 0x0f));
        robust(raw, static_cast<int>(rng() % 8), 16 * (1 + rng() % 64));
    }
    // null / empty
    {
        std::string why;
        CHECK(!bcpack::decode(nullptr, 5, 1, nullptr, 8, &why), "null accepted");
        uint8_t d[8];
        CHECK(!bcpack::decode(nullptr, 0, 1, d, 8, &why), "empty input accepted");
        CHECK(!bcpack_decode(nullptr, 0, d, 8, &why), "empty input accepted (auto)");
    }
    // timing on a larger synthetic-like failure path (auto mode on garbage of a 64 KiB chunk size)
    {
        std::vector<uint8_t> raw(48000);
        for (auto& b : raw) b = static_cast<uint8_t>(rng());
        std::vector<uint8_t> dst(65536);
        const auto t0 = std::chrono::steady_clock::now();
        std::string why;
        bcpack::decode(raw.data(), raw.size(), bcpack::kAuto, dst.data(), dst.size(), &why);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("auto-mode reject of 48 KB garbage: %.2f ms\n", ms);
    }
    std::printf("%d vectors, %s (%d failures)\n", n, g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
