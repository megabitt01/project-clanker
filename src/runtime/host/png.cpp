// Minimal PNG writer (stored/uncompressed deflate) for debug screenshots.
#include "png.h"

#include <cstdio>
#include <vector>

namespace rt {

static uint32_t crc_table[256];

static void crc_init()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> c;
    be32(c, uint32_t(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    uint32_t k = crc(c.data() + 4, c.size() - 4) ^ 0xFFFFFFFFu;
    be32(c, k);
    fwrite(c.data(), 1, c.size(), f);
}

bool write_png(const char *path, const uint8_t *rgba, int w, int h, int stride)
{
    if (!crc_table[1])
        crc_init();
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + y * stride, rgba + y * stride + w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t x : raw) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    be32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

} // namespace rt
