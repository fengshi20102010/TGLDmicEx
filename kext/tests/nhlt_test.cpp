// Host unit test for kext/TGLDmic/Nhlt.hpp against real NHLT dumps in captures/.
//   make -C kext test
#include "../TGLDmic/Nhlt.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> b;
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    uint8_t buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return b;
}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    struct Case { const char *path; uint32_t mics; uint8_t arrayType; } cases[] = {
        { "../captures/xiaoxinpro13-cml-2ch/NHLT.bin", 2, Nhlt::kArraySmallLinear2Ch },  // Lenovo XiaoXin Pro 13 2020
        { "../captures/run3-pcm6/fw/NHLT.bin", 4, 0 },                                   // HP EliteBook 840 G8
    };
    for (auto &c : cases) {
        auto b = load(c.path);
        CHECK(!b.empty(), "cannot read %s", c.path);
        if (b.empty()) continue;
        auto r = Nhlt::dmicInfo(b.data(), b.size());
        printf("%-48s mics %u array_type 0x%02x %s\n", c.path, r.mics, r.arrayType, r.fromFormats ? "(from formats)" : "");
        CHECK(r.mics == c.mics, "%s: mics %u, expected %u", c.path, r.mics, c.mics);
        if (c.arrayType) CHECK(r.arrayType == c.arrayType, "%s: array_type 0x%02x", c.path, r.arrayType);

        // Robustness: every truncation must stay in bounds and never report more mics than the full table.
        for (size_t cut = 0; cut < b.size(); cut += 7) {
            auto t = Nhlt::dmicInfo(b.data(), cut);
            CHECK(t.mics <= r.mics, "%s cut %zu: mics %u", c.path, cut, t.mics);
        }
        // Corrupted endpoint length must not crash or loop.
        auto bad = b; bad[37] = 0xFF; bad[38] = 0xFF; bad[39] = 0xFF; bad[40] = 0x7F;
        (void)Nhlt::dmicInfo(bad.data(), bad.size());
    }
    uint8_t junk[64]; memset(junk, 0, sizeof(junk));
    CHECK(Nhlt::dmicInfo(junk, sizeof(junk)).mics == 0, "non-NHLT data must give 0");
    CHECK(Nhlt::dmicInfo(nullptr, 100).mics == 0, "null must give 0");
    printf(failures ? "%d FAILURE(S)\n" : "all NHLT tests passed\n", failures);
    return failures ? 1 : 0;
}
