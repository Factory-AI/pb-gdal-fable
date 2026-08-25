#include "jpeg_ijg.h"

#include <cstring>

namespace
{

const int ZZ[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18,
                    11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
                    13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43,
                    36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
                    38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

const int LUM[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14,
                     19, 26, 58, 60, 55,  14,  13,  16,  24, 40, 57,
                     69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18,
                     22, 37, 56, 68, 109, 103, 77,  24,  35, 55, 64,
                     81, 104, 113, 92, 49, 64,  78,  87,  103, 121,
                     120, 101, 72, 92, 95, 98,  112, 100, 103, 99};

const int CHR[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99,
                     99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66,
                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// jcparam.c standard Huffman tables (Annex K)
const uint8_t STD_DC_BITS[2][17] = {
    {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}};
const uint8_t STD_DC_VALS[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t STD_AC_BITS[2][17] = {
    {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77}};
const uint8_t STD_AC_LUM_VALS[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41,
    0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91,
    0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2,
    0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const uint8_t STD_AC_CHR_VALS[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12,
    0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14,
    0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17,
    0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5,
    0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
    0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2,
    0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

const int32_t C0298 = 2446, C0390 = 3196, C0541 = 4433, C0765 = 6270,
              C0899 = 7373, C1175 = 9633, C1501 = 12299, C1847 = 15137,
              C1961 = 16069, C2053 = 16819, C2562 = 20995, C3072 = 25172;
const int CONST_BITS = 13, PASS1_BITS = 2;

inline int32_t descale(int32_t x, int n)
{
    return (x + ((int32_t)1 << (n - 1))) >> n;
}

void fdctIslow(int32_t *w)
{
    for (int i = 0; i < 8; i++)
    {
        int32_t *r = w + i * 8;
        int32_t t0 = r[0] + r[7], t7 = r[0] - r[7];
        int32_t t1 = r[1] + r[6], t6 = r[1] - r[6];
        int32_t t2 = r[2] + r[5], t5 = r[2] - r[5];
        int32_t t3 = r[3] + r[4], t4 = r[3] - r[4];
        int32_t t10 = t0 + t3, t13 = t0 - t3;
        int32_t t11 = t1 + t2, t12 = t1 - t2;
        r[0] = (t10 + t11) << PASS1_BITS;
        r[4] = (t10 - t11) << PASS1_BITS;
        int32_t z1 = (t12 + t13) * C0541;
        r[2] = descale(z1 + t13 * C0765, CONST_BITS - PASS1_BITS);
        r[6] = descale(z1 - t12 * C1847, CONST_BITS - PASS1_BITS);
        z1 = t4 + t7;
        int32_t z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7;
        int32_t z5 = (z3 + z4) * C1175;
        t4 *= C0298;
        t5 *= C2053;
        t6 *= C3072;
        t7 *= C1501;
        z1 *= -C0899;
        z2 *= -C2562;
        z3 *= -C1961;
        z4 *= -C0390;
        z3 += z5;
        z4 += z5;
        r[7] = descale(t4 + z1 + z3, CONST_BITS - PASS1_BITS);
        r[5] = descale(t5 + z2 + z4, CONST_BITS - PASS1_BITS);
        r[3] = descale(t6 + z2 + z3, CONST_BITS - PASS1_BITS);
        r[1] = descale(t7 + z1 + z4, CONST_BITS - PASS1_BITS);
    }
    for (int i = 0; i < 8; i++)
    {
        int32_t *c = w + i;
        int32_t t0 = c[8 * 0] + c[8 * 7], t7 = c[8 * 0] - c[8 * 7];
        int32_t t1 = c[8 * 1] + c[8 * 6], t6 = c[8 * 1] - c[8 * 6];
        int32_t t2 = c[8 * 2] + c[8 * 5], t5 = c[8 * 2] - c[8 * 5];
        int32_t t3 = c[8 * 3] + c[8 * 4], t4 = c[8 * 3] - c[8 * 4];
        int32_t t10 = t0 + t3, t13 = t0 - t3;
        int32_t t11 = t1 + t2, t12 = t1 - t2;
        c[8 * 0] = descale(t10 + t11, PASS1_BITS);
        c[8 * 4] = descale(t10 - t11, PASS1_BITS);
        int32_t z1 = (t12 + t13) * C0541;
        c[8 * 2] = descale(z1 + t13 * C0765, CONST_BITS + PASS1_BITS);
        c[8 * 6] = descale(z1 - t12 * C1847, CONST_BITS + PASS1_BITS);
        z1 = t4 + t7;
        int32_t z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7;
        int32_t z5 = (z3 + z4) * C1175;
        t4 *= C0298;
        t5 *= C2053;
        t6 *= C3072;
        t7 *= C1501;
        z1 *= -C0899;
        z2 *= -C2562;
        z3 *= -C1961;
        z4 *= -C0390;
        z3 += z5;
        z4 += z5;
        c[8 * 7] = descale(t4 + z1 + z3, CONST_BITS + PASS1_BITS);
        c[8 * 5] = descale(t5 + z2 + z4, CONST_BITS + PASS1_BITS);
        c[8 * 3] = descale(t6 + z2 + z3, CONST_BITS + PASS1_BITS);
        c[8 * 1] = descale(t7 + z1 + z4, CONST_BITS + PASS1_BITS);
    }
}

void quantizeBlock(const int32_t *w, const uint16_t *qt, int16_t *out)
{
    for (int i = 0; i < 64; i++)
    {
        int32_t q = (int32_t)qt[i] << 3;
        int32_t t = w[i];
        if (t < 0)
            t = -((-t + (q >> 1)) / q);
        else
            t = (t + (q >> 1)) / q;
        out[i] = (int16_t)t;
    }
}

inline int catBits(int v)
{
    unsigned a = v < 0 ? (unsigned)(-v) : (unsigned)v;
    int n = 0;
    while (a)
    {
        n++;
        a >>= 1;
    }
    return n;
}

void genOptimal(long freq[257], uint8_t bits[17],
                std::vector<uint8_t> &vals)
{
    int codesize[257];
    int others[257];
    memset(codesize, 0, sizeof(codesize));
    for (int i = 0; i < 257; i++)
        others[i] = -1;
    freq[256] = 1;
    for (;;)
    {
        int c1 = -1;
        long v = 1000000000L;
        for (int i = 0; i <= 256; i++)
            if (freq[i] && freq[i] <= v)
            {
                v = freq[i];
                c1 = i;
            }
        int c2 = -1;
        v = 1000000000L;
        for (int i = 0; i <= 256; i++)
            if (freq[i] && freq[i] <= v && i != c1)
            {
                v = freq[i];
                c2 = i;
            }
        if (c2 < 0)
            break;
        freq[c1] += freq[c2];
        freq[c2] = 0;
        codesize[c1]++;
        while (others[c1] >= 0)
        {
            c1 = others[c1];
            codesize[c1]++;
        }
        others[c1] = c2;
        codesize[c2]++;
        while (others[c2] >= 0)
        {
            c2 = others[c2];
            codesize[c2]++;
        }
    }
    int cnt[33];
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i <= 256; i++)
        if (codesize[i])
            cnt[codesize[i]]++;
    int i = 32;
    for (; i > 16; i--)
        while (cnt[i] > 0)
        {
            int j = i - 2;
            while (cnt[j] == 0)
                j--;
            cnt[i] -= 2;
            cnt[i - 1]++;
            cnt[j + 1] += 2;
            cnt[j]--;
        }
    while (cnt[i] == 0)
        i--;
    cnt[i]--;
    memset(bits, 0, 17);
    for (int l = 1; l <= 16; l++)
        bits[l] = (uint8_t)cnt[l];
    vals.clear();
    for (int l = 1; l <= 32; l++)
        for (int j = 0; j <= 255; j++)
            if (codesize[j] == l)
                vals.push_back((uint8_t)j);
}

struct HuffCode
{
    uint16_t code[256];
    uint8_t size[256];
};

void makeCodes(const uint8_t bits[17], const std::vector<uint8_t> &vals,
               HuffCode &hc)
{
    memset(hc.size, 0, sizeof(hc.size));
    uint16_t code = 0;
    size_t k = 0;
    for (int l = 1; l <= 16; l++)
    {
        for (int n = 0; n < bits[l]; n++)
        {
            hc.code[vals[k]] = code;
            hc.size[vals[k]] = (uint8_t)l;
            code++;
            k++;
        }
        code <<= 1;
    }
}

struct BitSink
{
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int nb = 0;
    void put(uint32_t code, int size)
    {
        acc = (acc << size) | code;
        nb += size;
        while (nb >= 8)
        {
            uint8_t b = (uint8_t)((acc >> (nb - 8)) & 0xFF);
            out.push_back(b);
            if (b == 0xFF)
                out.push_back(0);
            nb -= 8;
        }
        acc &= ((uint32_t)1 << nb) - 1;
    }
    void flush()
    {
        if (nb)
        {
            int pad = 8 - nb;
            acc = (acc << pad) | (((uint32_t)1 << pad) - 1);
            uint8_t b = (uint8_t)(acc & 0xFF);
            out.push_back(b);
            if (b == 0xFF)
                out.push_back(0);
            nb = 0;
            acc = 0;
        }
    }
};

void be16(std::vector<uint8_t> &v, unsigned x)
{
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xFF));
}

}  // namespace

int jpegZigzagIndex(int i)
{
    return ZZ[i];
}

std::vector<uint16_t> jpegQuantTable(bool chroma, int quality)
{
    if (quality <= 0)
        quality = 1;
    if (quality > 100)
        quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    const int *base = chroma ? CHR : LUM;
    std::vector<uint16_t> out(64);
    for (int i = 0; i < 64; i++)
    {
        long v = ((long)base[i] * scale + 50) / 100;
        if (v <= 0)
            v = 1;
        if (v > 32767)
            v = 32767;
        out[i] = (uint16_t)v;
    }
    return out;
}

namespace
{
void emitDqt(std::vector<uint8_t> &s,
             const std::vector<std::vector<uint16_t>> &qts)
{
    for (size_t tid = 0; tid < qts.size(); tid++)
    {
        const auto &qt = qts[tid];
        bool prec = false;
        for (uint16_t q : qt)
            if (q > 255)
                prec = true;
        s.push_back(0xFF);
        s.push_back(0xDB);
        be16(s, 2 + 1 + (prec ? 128 : 64));
        s.push_back((uint8_t)(((prec ? 1 : 0) << 4) | tid));
        for (int i = 0; i < 64; i++)
        {
            uint16_t q = qt[ZZ[i]];
            if (prec)
                s.push_back((uint8_t)(q >> 8));
            s.push_back((uint8_t)(q & 0xFF));
        }
    }
}
}  // namespace

std::vector<uint8_t> jpegTablesStream(
    const std::vector<std::vector<uint16_t>> &qts, bool withHuff,
    bool withQuant)
{
    std::vector<uint8_t> s = {0xFF, 0xD8};
    if (withQuant)
        emitDqt(s, qts);
    if (withHuff)
        for (size_t tid = 0; tid < qts.size() && tid < 2; tid++)
            for (int cls = 0; cls < 2; cls++)
            {
                const uint8_t *bits =
                    cls ? STD_AC_BITS[tid] : STD_DC_BITS[tid];
                const uint8_t *vals =
                    cls ? (tid ? STD_AC_CHR_VALS : STD_AC_LUM_VALS)
                        : STD_DC_VALS;
                int nv = 0;
                for (int l = 1; l <= 16; l++)
                    nv += bits[l];
                s.push_back(0xFF);
                s.push_back(0xC4);
                be16(s, 2 + 1 + 16 + (unsigned)nv);
                s.push_back((uint8_t)((cls << 4) | tid));
                for (int l = 1; l <= 16; l++)
                    s.push_back(bits[l]);
                s.insert(s.end(), vals, vals + nv);
            }
    s.push_back(0xFF);
    s.push_back(0xD9);
    return s;
}

void jpegRgbToYcc(uint8_t r, uint8_t g, uint8_t b, uint8_t &y,
                  uint8_t &cb, uint8_t &cr)
{
    const int32_t ONE_HALF = 1 << 15;
    const int32_t CBCR = 128 << 16;
    auto FIX = [](double x) { return (int32_t)(x * 65536 + 0.5); };
    y = (uint8_t)((FIX(0.29900) * r + FIX(0.58700) * g +
                   FIX(0.11400) * b + ONE_HALF) >>
                  16);
    cb = (uint8_t)((-FIX(0.16874) * r - FIX(0.33126) * g +
                    FIX(0.50000) * b + CBCR + ONE_HALF - 1) >>
                   16);
    cr = (uint8_t)((FIX(0.50000) * r - FIX(0.41869) * g -
                    FIX(0.08131) * b + CBCR + ONE_HALF - 1) >>
                   16);
}

std::vector<uint8_t> jpegH2v2Downsample(const std::vector<uint8_t> &in,
                                        int ow, int oh)
{
    std::vector<uint8_t> out((size_t)ow * oh);
    int iw = ow * 2;
    for (int j = 0; j < oh; j++)
    {
        const uint8_t *r0 = in.data() + (size_t)(2 * j) * iw;
        const uint8_t *r1 = r0 + iw;
        uint8_t *o = out.data() + (size_t)j * ow;
        int bias = 1;
        for (int i = 0; i < ow; i++)
        {
            o[i] = (uint8_t)((r0[2 * i] + r0[2 * i + 1] + r1[2 * i] +
                              r1[2 * i + 1] + bias) >>
                             2);
            bias ^= 3;
        }
    }
    return out;
}

std::vector<uint8_t> jpegStripStream(
    const std::vector<JpegScanComp> &comps, int w, int h, int ntables,
    const std::vector<std::vector<uint16_t>> &qts, bool stdHuff,
    bool embedQuant)
{
    const int ncomp = (int)comps.size();
    int maxh = 1, maxv = 1;
    for (const auto &c : comps)
    {
        if (c.hs > maxh)
            maxh = c.hs;
        if (c.vs > maxv)
            maxv = c.vs;
    }
    const int mcux = (w + maxh * 8 - 1) / (maxh * 8);
    const int mcuy = (h + maxv * 8 - 1) / (maxv * 8);

    // block-pad each component by edge replication
    std::vector<std::vector<uint8_t>> padded(ncomp);
    std::vector<int> wb(ncomp), hb(ncomp);
    for (int c = 0; c < ncomp; c++)
    {
        wb[c] = (comps[c].w + 7) / 8;
        hb[c] = (comps[c].h + 7) / 8;
        int tw = wb[c] * 8, th = hb[c] * 8;
        std::vector<uint8_t> a((size_t)tw * th);
        for (int j = 0; j < th; j++)
        {
            int sj = j < comps[c].h ? j : comps[c].h - 1;
            const uint8_t *src =
                comps[c].samples.data() + (size_t)sj * comps[c].w;
            uint8_t *dst = a.data() + (size_t)j * tw;
            memcpy(dst, src, comps[c].w);
            for (int i = comps[c].w; i < tw; i++)
                dst[i] = src[comps[c].w - 1];
        }
        padded[c] = std::move(a);
    }

    // quantized blocks per component
    std::vector<std::vector<int16_t>> blocks(ncomp);
    for (int c = 0; c < ncomp; c++)
    {
        const uint16_t *qt = qts[comps[c].qtsel].data();
        blocks[c].resize((size_t)wb[c] * hb[c] * 64);
        int tw = wb[c] * 8;
        for (int by = 0; by < hb[c]; by++)
            for (int bx = 0; bx < wb[c]; bx++)
            {
                int32_t wk[64];
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++)
                        wk[j * 8 + i] =
                            (int32_t)padded[c][(size_t)(by * 8 + j) * tw +
                                               bx * 8 + i] -
                            128;
                fdctIslow(wk);
                quantizeBlock(
                    wk, qt,
                    blocks[c].data() + ((size_t)by * wb[c] + bx) * 64);
            }
    }

    // walk MCUs twice: gather statistics, then emit
    struct Sym
    {
        // kind 0 = DC (sym,val=diff), 1 = AC run/size with value,
        // 2 = AC bare (ZRL or EOB)
        uint8_t kind, tab, sym;
        int16_t val;
    };
    std::vector<long> dcFreq((size_t)ntables * 257, 0),
        acFreq((size_t)ntables * 257, 0);
    std::vector<Sym> syms;
    int prevdc[4] = {0, 0, 0, 0};
    int16_t prevBlockDc = 0;
    int16_t zz[64];
    for (int my = 0; my < mcuy; my++)
        for (int mx = 0; mx < mcux; mx++)
            for (int c = 0; c < ncomp; c++)
                for (int by = 0; by < comps[c].vs; by++)
                    for (int bx = 0; bx < comps[c].hs; bx++)
                    {
                        int gx = mx * comps[c].hs + bx;
                        int gy = my * comps[c].vs + by;
                        if (gx < wb[c] && gy < hb[c])
                        {
                            const int16_t *blk =
                                blocks[c].data() +
                                ((size_t)gy * wb[c] + gx) * 64;
                            for (int i = 0; i < 64; i++)
                                zz[i] = blk[ZZ[i]];
                        }
                        else
                        {
                            // dummy block: DC of the previous block in
                            // the MCU, all AC zero
                            memset(zz, 0, sizeof(zz));
                            zz[0] = prevBlockDc;
                        }
                        prevBlockDc = zz[0];
                        uint8_t t = (uint8_t)comps[c].tabsel;
                        int diff = zz[0] - prevdc[c];
                        prevdc[c] = zz[0];
                        int n = catBits(diff);
                        dcFreq[(size_t)t * 257 + n]++;
                        syms.push_back(
                            {0, t, (uint8_t)n, (int16_t)diff});
                        int run = 0;
                        for (int k = 1; k < 64; k++)
                        {
                            if (zz[k] == 0)
                            {
                                run++;
                                continue;
                            }
                            while (run > 15)
                            {
                                acFreq[(size_t)t * 257 + 0xF0]++;
                                syms.push_back({2, t, 0xF0, 0});
                                run -= 16;
                            }
                            int nn = catBits(zz[k]);
                            uint8_t s = (uint8_t)((run << 4) + nn);
                            acFreq[(size_t)t * 257 + s]++;
                            syms.push_back({1, t, s, zz[k]});
                            run = 0;
                        }
                        if (run)
                        {
                            acFreq[(size_t)t * 257 + 0]++;
                            syms.push_back({2, t, 0, 0});
                        }
                    }

    std::vector<uint8_t> dcBits((size_t)ntables * 17),
        acBits((size_t)ntables * 17);
    std::vector<std::vector<uint8_t>> dcVals(ntables), acVals(ntables);
    std::vector<HuffCode> dcCodes(ntables), acCodes(ntables);
    for (int t = 0; t < ntables; t++)
    {
        if (stdHuff)
        {
            int ti = t < 2 ? t : 1;
            memcpy(dcBits.data() + (size_t)t * 17, STD_DC_BITS[ti], 17);
            memcpy(acBits.data() + (size_t)t * 17, STD_AC_BITS[ti], 17);
            dcVals[t].assign(STD_DC_VALS, STD_DC_VALS + 12);
            const uint8_t *av = ti ? STD_AC_CHR_VALS : STD_AC_LUM_VALS;
            acVals[t].assign(av, av + 162);
        }
        else
        {
            genOptimal(dcFreq.data() + (size_t)t * 257,
                       dcBits.data() + (size_t)t * 17, dcVals[t]);
            genOptimal(acFreq.data() + (size_t)t * 257,
                       acBits.data() + (size_t)t * 17, acVals[t]);
        }
        makeCodes(dcBits.data() + (size_t)t * 17, dcVals[t], dcCodes[t]);
        makeCodes(acBits.data() + (size_t)t * 17, acVals[t], acCodes[t]);
    }

    BitSink sink;
    for (const Sym &s : syms)
    {
        if (s.kind == 0)
        {
            sink.put(dcCodes[s.tab].code[s.sym],
                     dcCodes[s.tab].size[s.sym]);
            int n = s.sym;
            if (n)
            {
                int v = s.val >= 0 ? s.val : s.val + (1 << n) - 1;
                sink.put((uint32_t)v & (((uint32_t)1 << n) - 1), n);
            }
        }
        else if (s.kind == 2)
            sink.put(acCodes[s.tab].code[s.sym],
                     acCodes[s.tab].size[s.sym]);
        else
        {
            sink.put(acCodes[s.tab].code[s.sym],
                     acCodes[s.tab].size[s.sym]);
            int n = s.sym & 15;
            int v = s.val >= 0 ? s.val : s.val + (1 << n) - 1;
            sink.put((uint32_t)v & (((uint32_t)1 << n) - 1), n);
        }
    }
    sink.flush();

    bool baseline = true;
    for (const auto &c : comps)
        for (uint16_t q : qts[c.qtsel])
            if (q > 255)
                baseline = false;

    std::vector<uint8_t> s = {0xFF, 0xD8};
    if (embedQuant)
        emitDqt(s, qts);
    s.push_back(0xFF);
    s.push_back((uint8_t)(baseline ? 0xC0 : 0xC1));
    be16(s, 8 + 3 * ncomp);
    s.push_back(8);
    be16(s, (unsigned)h);
    be16(s, (unsigned)w);
    s.push_back((uint8_t)ncomp);
    for (const auto &c : comps)
    {
        s.push_back(c.id);
        s.push_back((uint8_t)((c.hs << 4) | c.vs));
        s.push_back((uint8_t)c.qtsel);
    }
    if (!stdHuff)
        for (int t = 0; t < ntables; t++)
        {
            for (int cls = 0; cls < 2; cls++)
            {
                const uint8_t *bits =
                    (cls ? acBits.data() : dcBits.data()) +
                    (size_t)t * 17;
                const std::vector<uint8_t> &vals =
                    cls ? acVals[t] : dcVals[t];
                s.push_back(0xFF);
                s.push_back(0xC4);
                be16(s, 2 + 1 + 16 + (unsigned)vals.size());
                s.push_back((uint8_t)((cls << 4) | t));
                for (int l = 1; l <= 16; l++)
                    s.push_back(bits[l]);
                s.insert(s.end(), vals.begin(), vals.end());
            }
        }
    s.push_back(0xFF);
    s.push_back(0xDA);
    be16(s, 6 + 2 * (unsigned)ncomp);
    s.push_back((uint8_t)ncomp);
    for (const auto &c : comps)
    {
        s.push_back(c.id);
        s.push_back((uint8_t)((c.tabsel << 4) | c.tabsel));
    }
    s.push_back(0);
    s.push_back(63);
    s.push_back(0);
    s.insert(s.end(), sink.out.begin(), sink.out.end());
    s.push_back(0xFF);
    s.push_back(0xD9);
    return s;
}

// ------------------------------------------------------------------
// decoder
// ------------------------------------------------------------------

namespace
{

// post-IDCT limiter: jdmaster's range table viewed from the
// IDCT_range_limit pointer, indexed with the 1023 mask
const uint8_t *idctRangeLimit()
{
    static uint8_t t[1024];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 128; i++)
            t[i] = (uint8_t)(i + 128);
        for (int i = 128; i < 512; i++)
            t[i] = 255;
        for (int i = 512; i < 896; i++)
            t[i] = 0;
        for (int i = 896; i < 1024; i++)
            t[i] = (uint8_t)(i - 896);
        init = true;
    }
    return t;
}

struct DHuffTable
{
    bool valid = false;
    int mincode[17], maxcode[18], valptr[17];
    std::vector<uint8_t> vals;

    void derive(const uint8_t bits[17])
    {
        int code = 0, p = 0;
        for (int l = 1; l <= 16; l++)
        {
            if (bits[l])
            {
                valptr[l] = p;
                mincode[l] = code;
                p += bits[l];
                code += bits[l];
                maxcode[l] = code - 1;
            }
            else
                maxcode[l] = -1;
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        valid = true;
    }
};

struct BitSource
{
    const uint8_t *p, *end;
    uint32_t acc = 0;
    int nbits = 0;
    bool bad = false;

    int getBit()
    {
        if (nbits == 0)
        {
            int b = nextByte();
            if (b < 0)
            {
                bad = true;
                return 0;
            }
            acc = (uint32_t)b;
            nbits = 8;
        }
        nbits--;
        return (int)((acc >> nbits) & 1);
    }

    int getBits(int n)
    {
        int v = 0;
        for (int i = 0; i < n; i++)
            v = (v << 1) | getBit();
        return v;
    }

    int nextByte()
    {
        while (p < end)
        {
            uint8_t b = *p++;
            if (b != 0xFF)
                return b;
            if (p < end && *p == 0x00)
            {
                p++;
                return 0xFF;
            }
            // marker: entropy data exhausted
            p--;
            return -1;
        }
        return -1;
    }
};

int huffDecode(BitSource &src, const DHuffTable &tb)
{
    int code = src.getBit();
    int l = 1;
    while (code > tb.maxcode[l])
    {
        code = (code << 1) | src.getBit();
        l++;
        if (l > 16)
            return -1;
    }
    return tb.vals[(size_t)tb.valptr[l] + (size_t)(code - tb.mincode[l])];
}

inline int huffExtend(int v, int s)
{
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

// jpeg_idct_islow: coef block (natural order) x quant table -> 8x8
// samples written to dst with given stride
void idctIslow(const int16_t *coef, const uint16_t *qt, uint8_t *dst,
               size_t stride)
{
    const uint8_t *range = idctRangeLimit();
    int32_t ws[64];
    for (int c = 0; c < 8; c++)
    {
        const int16_t *in = coef + c;
        const uint16_t *q = qt + c;
        int32_t *w = ws + c;
        if (in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 &&
            in[40] == 0 && in[48] == 0 && in[56] == 0)
        {
            int32_t dc = (int32_t)in[0] * q[0] << PASS1_BITS;
            for (int r = 0; r < 8; r++)
                w[r * 8] = dc;
            continue;
        }
        int32_t z2 = (int32_t)in[16] * q[16];
        int32_t z3 = (int32_t)in[48] * q[48];
        int32_t z1 = (z2 + z3) * C0541;
        int32_t tmp2 = z1 + z3 * (-C1847);
        int32_t tmp3 = z1 + z2 * C0765;
        z2 = (int32_t)in[0] * q[0];
        z3 = (int32_t)in[32] * q[32];
        int32_t tmp0 = (z2 + z3) << CONST_BITS;
        int32_t tmp1 = (z2 - z3) << CONST_BITS;
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = (int32_t)in[56] * q[56];
        tmp1 = (int32_t)in[40] * q[40];
        tmp2 = (int32_t)in[24] * q[24];
        tmp3 = (int32_t)in[8] * q[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * C1175;
        tmp0 *= C0298;
        tmp1 *= C2053;
        tmp2 *= C3072;
        tmp3 *= C1501;
        z1 *= -C0899;
        z2 *= -C2562;
        z3 *= -C1961;
        z4 *= -C0390;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        w[0] = descale(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        w[56] = descale(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        w[8] = descale(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        w[48] = descale(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        w[16] = descale(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        w[40] = descale(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        w[24] = descale(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        w[32] = descale(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }
    for (int r = 0; r < 8; r++)
    {
        int32_t *w = ws + r * 8;
        uint8_t *out = dst + (size_t)r * stride;
        if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 &&
            w[5] == 0 && w[6] == 0 && w[7] == 0)
        {
            uint8_t dc =
                range[descale(w[0], PASS1_BITS + 3) & 1023];
            for (int c = 0; c < 8; c++)
                out[c] = dc;
            continue;
        }
        int32_t z2 = w[2], z3 = w[6];
        int32_t z1 = (z2 + z3) * C0541;
        int32_t tmp2 = z1 + z3 * (-C1847);
        int32_t tmp3 = z1 + z2 * C0765;
        int32_t tmp0 = (w[0] + w[4]) << CONST_BITS;
        int32_t tmp1 = (w[0] - w[4]) << CONST_BITS;
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = w[7];
        tmp1 = w[5];
        tmp2 = w[3];
        tmp3 = w[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * C1175;
        tmp0 *= C0298;
        tmp1 *= C2053;
        tmp2 *= C3072;
        tmp3 *= C1501;
        z1 *= -C0899;
        z2 *= -C2562;
        z3 *= -C1961;
        z4 *= -C0390;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        const int fin = CONST_BITS + PASS1_BITS + 3;
        out[0] = range[descale(tmp10 + tmp3, fin) & 1023];
        out[7] = range[descale(tmp10 - tmp3, fin) & 1023];
        out[1] = range[descale(tmp11 + tmp2, fin) & 1023];
        out[6] = range[descale(tmp11 - tmp2, fin) & 1023];
        out[2] = range[descale(tmp12 + tmp1, fin) & 1023];
        out[5] = range[descale(tmp12 - tmp1, fin) & 1023];
        out[3] = range[descale(tmp13 + tmp0, fin) & 1023];
        out[4] = range[descale(tmp13 - tmp0, fin) & 1023];
    }
}

struct DecComp
{
    uint8_t id = 0;
    int hs = 1, vs = 1, tq = 0;
    int dcSel = 0, acSel = 0;
    int compW = 0, compH = 0;  // padded plane dims
    std::vector<uint8_t> plane;
    int pred = 0;
};

struct DecState
{
    uint16_t qt[4][64];  // natural order
    bool qtSet[4] = {false, false, false, false};
    DHuffTable dcTab[4], acTab[4];
    int w = 0, h = 0;
    std::vector<DecComp> comps;
    bool haveSof = false;
};

bool parseSegments(DecState &st, const uint8_t *d, size_t n,
                   size_t &sosPayload)
{
    sosPayload = 0;
    if (n < 2 || d[0] != 0xFF || d[1] != 0xD8)
        return false;
    size_t i = 2;
    while (i + 2 <= n)
    {
        if (d[i] != 0xFF)
            return false;
        uint8_t m = d[i + 1];
        if (m == 0xD9)
            return true;
        if (i + 4 > n)
            return false;
        size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (len < 2 || i + 2 + len > n)
            return false;
        const uint8_t *seg = d + i + 4;
        size_t segLen = len - 2;
        if (m == 0xDB)
        {
            size_t k = 0;
            while (k < segLen)
            {
                int prec = seg[k] >> 4, id = seg[k] & 15;
                k++;
                if (id > 3 || k + (prec ? 128 : 64) > segLen)
                    return false;
                for (int j = 0; j < 64; j++)
                {
                    unsigned v =
                        prec ? ((unsigned)seg[k + 2 * j] << 8) |
                                   seg[k + 2 * j + 1]
                             : seg[k + j];
                    st.qt[id][ZZ[j]] = (uint16_t)v;
                }
                st.qtSet[id] = true;
                k += prec ? 128 : 64;
            }
        }
        else if (m == 0xC4)
        {
            size_t k = 0;
            while (k + 17 <= segLen)
            {
                int cls = seg[k] >> 4, id = seg[k] & 15;
                if (id > 3)
                    return false;
                uint8_t bits[17];
                bits[0] = 0;
                int total = 0;
                for (int l = 1; l <= 16; l++)
                {
                    bits[l] = seg[k + l];
                    total += bits[l];
                }
                if (k + 17 + (size_t)total > segLen)
                    return false;
                DHuffTable &tb = cls ? st.acTab[id] : st.dcTab[id];
                tb.vals.assign(seg + k + 17, seg + k + 17 + total);
                tb.derive(bits);
                k += 17 + (size_t)total;
            }
        }
        else if (m == 0xC0 || m == 0xC1)
        {
            if (segLen < 6 || seg[0] != 8)
                return false;
            st.h = ((int)seg[1] << 8) | seg[2];
            st.w = ((int)seg[3] << 8) | seg[4];
            int nc = seg[5];
            if (nc < 1 || nc > 4 || segLen < 6 + (size_t)nc * 3)
                return false;
            st.comps.clear();
            for (int c = 0; c < nc; c++)
            {
                DecComp dc;
                dc.id = seg[6 + c * 3];
                dc.hs = seg[7 + c * 3] >> 4;
                dc.vs = seg[7 + c * 3] & 15;
                dc.tq = seg[8 + c * 3];
                if (dc.hs < 1 || dc.hs > 4 || dc.vs < 1 || dc.vs > 4 ||
                    dc.tq > 3)
                    return false;
                st.comps.push_back(dc);
            }
            st.haveSof = true;
        }
        else if (m == 0xDA)
        {
            if (!st.haveSof || segLen < 1)
                return false;
            int ns = seg[0];
            if (ns != (int)st.comps.size() ||
                segLen < 1 + (size_t)ns * 2 + 3)
                return false;
            for (int c = 0; c < ns; c++)
            {
                uint8_t cs = seg[1 + c * 2];
                bool found = false;
                for (auto &comp : st.comps)
                    if (comp.id == cs)
                    {
                        comp.dcSel = seg[2 + c * 2] >> 4;
                        comp.acSel = seg[2 + c * 2] & 15;
                        found = true;
                    }
                if (!found)
                    return false;
            }
            sosPayload = i + 2 + len;
            return true;
        }
        else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 &&
                 m != 0xCC)
            return false;  // non-baseline frame
        i += 2 + len;
    }
    return false;
}

}  // namespace

bool jpegDecodeStream(const uint8_t *data, size_t len,
                      const uint8_t *tables, size_t tablesLen,
                      bool yccToRgb, JpegDecoded &out)
{
    DecState st;
    if (tables && tablesLen)
    {
        size_t dummy = 0;
        if (!parseSegments(st, tables, tablesLen, dummy))
            return false;
    }
    size_t sos = 0;
    if (!parseSegments(st, data, len, sos) || !st.haveSof || sos == 0)
        return false;

    int hmax = 1, vmax = 1;
    for (const auto &c : st.comps)
    {
        hmax = std::max(hmax, c.hs);
        vmax = std::max(vmax, c.vs);
    }
    const int mcusX = (st.w + 8 * hmax - 1) / (8 * hmax);
    const int mcusY = (st.h + 8 * vmax - 1) / (8 * vmax);
    for (auto &c : st.comps)
    {
        if (!st.qtSet[c.tq] || !st.dcTab[c.dcSel].valid ||
            !st.acTab[c.acSel].valid)
            return false;
        c.compW = mcusX * 8 * c.hs;
        c.compH = mcusY * 8 * c.vs;
        c.plane.assign((size_t)c.compW * c.compH, 0);
        c.pred = 0;
    }

    BitSource src{data + sos, data + len};
    int16_t coef[64];
    for (int my = 0; my < mcusY; my++)
        for (int mx = 0; mx < mcusX; mx++)
            for (auto &c : st.comps)
                for (int by = 0; by < c.vs; by++)
                    for (int bx = 0; bx < c.hs; bx++)
                    {
                        memset(coef, 0, sizeof(coef));
                        int t = huffDecode(src, st.dcTab[c.dcSel]);
                        if (t < 0 || src.bad)
                            return false;
                        int diff = t ? huffExtend(src.getBits(t), t) : 0;
                        c.pred += diff;
                        coef[0] = (int16_t)c.pred;
                        for (int k = 1; k < 64;)
                        {
                            int rs = huffDecode(src, st.acTab[c.acSel]);
                            if (rs < 0 || src.bad)
                                return false;
                            int s = rs & 15, r = rs >> 4;
                            if (s == 0)
                            {
                                if (r != 15)
                                    break;
                                k += 16;
                                continue;
                            }
                            k += r;
                            if (k > 63)
                                return false;
                            coef[ZZ[k]] =
                                (int16_t)huffExtend(src.getBits(s), s);
                            k++;
                        }
                        int px = (mx * c.hs + bx) * 8;
                        int py = (my * c.vs + by) * 8;
                        idctIslow(coef, st.qt[c.tq],
                                  c.plane.data() +
                                      (size_t)py * c.compW + px,
                                  (size_t)c.compW);
                    }
    if (src.bad)
        return false;

    out.w = st.w;
    out.h = st.h;
    const int nc = (int)st.comps.size();
    out.ncomp = nc;
    out.pixels.assign((size_t)st.w * st.h * nc, 0);
    // full-resolution planes per component (jdsample fancy triangle
    // upsampling, the libjpeg default that libtiff keeps enabled)
    std::vector<std::vector<uint8_t>> up(nc);
    for (int ci = 0; ci < nc; ci++)
    {
        const DecComp &c = st.comps[ci];
        const int dw = (st.w * c.hs + hmax - 1) / hmax;
        const int dh = (st.h * c.vs + vmax - 1) / vmax;
        std::vector<uint8_t> &plane = up[ci];
        if (c.hs == hmax && c.vs == vmax)
        {
            plane.assign((size_t)st.w * st.h, 0);
            for (int y = 0; y < st.h; y++)
                memcpy(plane.data() + (size_t)y * st.w,
                       c.plane.data() + (size_t)y * c.compW, st.w);
            continue;
        }
        if (2 * c.hs == hmax && 2 * c.vs == vmax)
        {
            // h2v2_fancy_upsample
            const int ow = dw * 2;
            std::vector<uint8_t> full((size_t)ow * dh * 2, 0);
            for (int iy = 0; iy < dh; iy++)
            {
                for (int v = 0; v < 2; v++)
                {
                    const uint8_t *in0 =
                        c.plane.data() + (size_t)iy * c.compW;
                    int near = v == 0 ? iy - 1 : iy + 1;
                    if (near < 0)
                        near = 0;
                    if (near >= dh)
                        near = dh - 1;
                    const uint8_t *in1 =
                        c.plane.data() + (size_t)near * c.compW;
                    uint8_t *op =
                        full.data() + (size_t)(iy * 2 + v) * ow;
                    if (dw == 1)
                    {
                        int t = in0[0] * 3 + in1[0];
                        op[0] = (uint8_t)((t * 4 + 8) >> 4);
                        op[1] = (uint8_t)((t * 4 + 7) >> 4);
                        continue;
                    }
                    int thisc = in0[0] * 3 + in1[0];
                    int nextc = in0[1] * 3 + in1[1];
                    *op++ = (uint8_t)((thisc * 4 + 8) >> 4);
                    *op++ = (uint8_t)((thisc * 3 + nextc + 7) >> 4);
                    int lastc = thisc;
                    thisc = nextc;
                    for (int col = 2; col < dw; col++)
                    {
                        nextc = in0[col] * 3 + in1[col];
                        *op++ =
                            (uint8_t)((thisc * 3 + lastc + 8) >> 4);
                        *op++ =
                            (uint8_t)((thisc * 3 + nextc + 7) >> 4);
                        lastc = thisc;
                        thisc = nextc;
                    }
                    *op++ = (uint8_t)((thisc * 3 + lastc + 8) >> 4);
                    *op = (uint8_t)((thisc * 4 + 7) >> 4);
                }
            }
            plane.assign((size_t)st.w * st.h, 0);
            for (int y = 0; y < st.h; y++)
                memcpy(plane.data() + (size_t)y * st.w,
                       full.data() + (size_t)y * ow, st.w);
            continue;
        }
        if (2 * c.hs == hmax && c.vs == vmax)
        {
            // h2v1_fancy_upsample
            const int ow = dw * 2;
            std::vector<uint8_t> full((size_t)ow * dh, 0);
            for (int iy = 0; iy < dh; iy++)
            {
                const uint8_t *in = c.plane.data() + (size_t)iy * c.compW;
                uint8_t *op = full.data() + (size_t)iy * ow;
                if (dw == 1)
                {
                    op[0] = in[0];
                    op[1] = in[0];
                    continue;
                }
                *op++ = in[0];
                *op++ = (uint8_t)((in[0] * 3 + in[1] + 2) >> 2);
                for (int col = 1; col < dw - 1; col++)
                {
                    int iv = in[col] * 3;
                    *op++ = (uint8_t)((iv + in[col - 1] + 1) >> 2);
                    *op++ = (uint8_t)((iv + in[col + 1] + 2) >> 2);
                }
                *op++ =
                    (uint8_t)((in[dw - 1] * 3 + in[dw - 2] + 1) >> 2);
                *op = in[dw - 1];
            }
            plane.assign((size_t)st.w * st.h, 0);
            for (int y = 0; y < st.h; y++)
                memcpy(plane.data() + (size_t)y * st.w,
                       full.data() + (size_t)y * ow, st.w);
            continue;
        }
        // generic box replication fallback
        plane.assign((size_t)st.w * st.h, 0);
        for (int y = 0; y < st.h; y++)
        {
            const uint8_t *sp =
                c.plane.data() + (size_t)(y * c.vs / vmax) * c.compW;
            for (int x = 0; x < st.w; x++)
                plane[(size_t)y * st.w + x] = sp[x * c.hs / hmax];
        }
    }
    if (yccToRgb && nc == 3)
    {
        // jdcolor build_ycc_rgb_table integer conversion
        static int crr[256], cbb[256], crg[256], cbg[256];
        static bool init = false;
        if (!init)
        {
            for (int i = 0; i < 256; i++)
            {
                int x = i - 128;
                crr[i] = (int)((91881 * (long long)x + 32768) >> 16);
                cbb[i] = (int)((116130 * (long long)x + 32768) >> 16);
                crg[i] = -46802 * x;
                cbg[i] = -22554 * x + 32768;
            }
            init = true;
        }
        auto clamp8 = [](int v)
        {
            return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        };
        for (size_t i = 0; i < (size_t)st.w * st.h; i++)
        {
            int lum = up[0][i], u = up[1][i], v = up[2][i];
            out.pixels[i * 3] = clamp8(lum + crr[v]);
            out.pixels[i * 3 + 1] =
                clamp8(lum + ((cbg[u] + crg[v]) >> 16));
            out.pixels[i * 3 + 2] = clamp8(lum + cbb[u]);
        }
        return true;
    }
    for (int ci = 0; ci < nc; ci++)
        for (size_t i = 0; i < (size_t)st.w * st.h; i++)
            out.pixels[i * nc + ci] = up[ci][i];
    return true;
}
