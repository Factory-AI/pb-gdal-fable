// minimal libzstd declarations (no -dev headers on this system); the
// streaming API is required: libtiff's zstd codec streams, so frames
// carry no content-size header, unlike ZSTD_compress output
#pragma once
#include <cstddef>

extern "C"
{
    typedef struct
    {
        const void *src;
        size_t size;
        size_t pos;
    } ZSTD_inBuffer;
    typedef struct
    {
        void *dst;
        size_t size;
        size_t pos;
    } ZSTD_outBuffer;

    typedef struct ZSTD_CStream_s ZSTD_CStream;
    ZSTD_CStream *ZSTD_createCStream(void);
    size_t ZSTD_freeCStream(ZSTD_CStream *);
    size_t ZSTD_initCStream(ZSTD_CStream *, int level);
    size_t ZSTD_compressStream(ZSTD_CStream *, ZSTD_outBuffer *,
                               ZSTD_inBuffer *);
    size_t ZSTD_endStream(ZSTD_CStream *, ZSTD_outBuffer *);

    typedef struct ZSTD_DStream_s ZSTD_DStream;
    ZSTD_DStream *ZSTD_createDStream(void);
    size_t ZSTD_freeDStream(ZSTD_DStream *);
    size_t ZSTD_initDStream(ZSTD_DStream *);
    size_t ZSTD_decompressStream(ZSTD_DStream *, ZSTD_outBuffer *,
                                 ZSTD_inBuffer *);

    unsigned ZSTD_isError(size_t code);
    size_t ZSTD_compressBound(size_t srcSize);
}
