#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// WEBP codec over the system libwebp, mirroring the reference's
// tif_webp.c usage: lossy = preset-default config at the given quality,
// lossless = use_argb import at quality 100, exact off
std::vector<uint8_t> webpEncodeBlock(const uint8_t *rgb, int w, int h,
                                     int spp, int level, bool lossless);
bool webpDecodeBlock(const uint8_t *data, size_t len, int w, int h,
                     int spp, uint8_t *out);
