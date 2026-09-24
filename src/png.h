// Minimal PNG writer (stored/uncompressed deflate) for scripted screenshots.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline bool write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba_bottom_up) {
    auto crc = [](const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
        static uint32_t t[256];
        static bool init = false;
        if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; t[i] = k; } init = true; }
        for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
        return c;
    };
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) { const uint8_t* p = &rgba_bottom_up[(size_t(y) * w + x) * 4]; raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        if (last) break;
    }
    uint32_t ad = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back(uint8_t(ad >> (8 * i)));
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [&](uint32_t v) { uint8_t q[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; fwrite(q, 1, 4, f); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        be32(uint32_t(d.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        be32(crc(td.data(), td.size()) ^ 0xffffffffu);
    };
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
    return true;
}
