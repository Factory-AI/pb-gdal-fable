#include "webp_shim.h"

#include <cstring>

// local ABI declarations for libwebp 1.2 (WEBP_ENCODER_ABI_VERSION
// 0x020f), field layout copied from upstream webp/encode.h
extern "C"
{
    struct WebPConfig
    {
        int lossless;
        float quality;
        int method;
        int image_hint;
        int target_size;
        float target_PSNR;
        int segments;
        int sns_strength;
        int filter_strength;
        int filter_sharpness;
        int filter_type;
        int autofilter;
        int alpha_compression;
        int alpha_filtering;
        int alpha_quality;
        int pass;
        int show_compressed;
        int preprocessing;
        int partitions;
        int partition_limit;
        int emulate_jpeg_size;
        int thread_level;
        int low_memory;
        int near_lossless;
        int exact;
        int use_delta_palette;
        int use_sharp_yuv;
        int qmin;
        int qmax;
    };
    struct WebPPicture;
    typedef int (*WebPWriterFunction)(const uint8_t *, size_t,
                                      const WebPPicture *);
    struct WebPPicture
    {
        int use_argb;
        int colorspace;
        int width, height;
        uint8_t *y, *u, *v;
        int y_stride, uv_stride;
        uint8_t *a;
        int a_stride;
        uint32_t pad1[2];
        uint32_t *argb;
        int argb_stride;
        uint32_t pad2[3];
        WebPWriterFunction writer;
        void *custom_ptr;
        int extra_info_type;
        uint8_t *extra_info;
        void *stats;
        int error_code;
        int (*progress_hook)(int, const WebPPicture *);
        void *user_data;
        uint32_t pad3[3];
        uint8_t *pad4;
        uint8_t *pad5;
        uint32_t pad6[8];
        void *memory_;
        void *memory_argb_;
        void *pad7[2];
    };
    struct WebPMemoryWriter
    {
        uint8_t *mem;
        size_t size;
        size_t max_size;
        uint32_t pad[1];
    };
    int WebPConfigInitInternal(WebPConfig *, int preset, float quality,
                               int abi);
    int WebPPictureInitInternal(WebPPicture *, int abi);
    int WebPPictureImportRGB(WebPPicture *, const uint8_t *, int stride);
    int WebPPictureImportRGBA(WebPPicture *, const uint8_t *, int stride);
    int WebPEncode(const WebPConfig *, WebPPicture *);
    void WebPPictureFree(WebPPicture *);
    void WebPMemoryWriterInit(WebPMemoryWriter *);
    void WebPMemoryWriterClear(WebPMemoryWriter *);
    int WebPMemoryWrite(const uint8_t *, size_t, const WebPPicture *);
    uint8_t *WebPDecodeRGBInto(const uint8_t *, size_t, uint8_t *out,
                               size_t out_size, int stride);
    uint8_t *WebPDecodeRGBAInto(const uint8_t *, size_t, uint8_t *out,
                                size_t out_size, int stride);
}

namespace
{
const int kWebPAbi = 0x020f;
}

std::vector<uint8_t> webpEncodeBlock(const uint8_t *rgb, int w, int h,
                                     int spp, int level, bool lossless)
{
    WebPConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    if (!WebPConfigInitInternal(&cfg, 0,
                                (float)(lossless ? 100 : level), kWebPAbi))
        return {};
    if (lossless)
        cfg.lossless = 1;
    WebPPicture pic;
    memset(&pic, 0, sizeof pic);
    if (!WebPPictureInitInternal(&pic, kWebPAbi))
        return {};
    pic.width = w;
    pic.height = h;
    if (lossless)
        pic.use_argb = 1;
    WebPMemoryWriter mw;
    WebPMemoryWriterInit(&mw);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &mw;
    int ok = spp == 4 ? WebPPictureImportRGBA(&pic, rgb, w * 4)
                      : WebPPictureImportRGB(&pic, rgb, w * 3);
    std::vector<uint8_t> out;
    if (ok && WebPEncode(&cfg, &pic))
        out.assign(mw.mem, mw.mem + mw.size);
    WebPMemoryWriterClear(&mw);
    WebPPictureFree(&pic);
    return out;
}

bool webpDecodeBlock(const uint8_t *data, size_t len, int w, int h,
                     int spp, uint8_t *out)
{
    size_t need = (size_t)w * h * spp;
    if (spp == 4)
        return WebPDecodeRGBAInto(data, len, out, need, w * 4) != nullptr;
    return WebPDecodeRGBInto(data, len, out, need, w * 3) != nullptr;
}
