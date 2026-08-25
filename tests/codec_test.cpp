// standalone encoder harness: reads raw bytes from a file, writes the
// encoded blob to stdout. usage: codec_test lzw|packbits|deflate:<lvl> <file> [rowbytes]
#include "../src/gtiff_write.cpp"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    std::string mode = argv[1];
    std::string content;
    if (!readFileToString(argv[2], content))
        return 3;
    std::vector<uint8_t> in(content.begin(), content.end());
    std::vector<uint8_t> out;
    if (mode == "lzw")
    {
        LzwEncoder enc;
        out = enc.encode(in.data(), in.size());
    }
    else if (mode == "packbits")
    {
        long rowBytes = argc > 3 ? atol(argv[3]) : (long)in.size();
        for (size_t r = 0; r < in.size(); r += rowBytes)
        {
            long n = (long)std::min((size_t)rowBytes, in.size() - r);
            packBitsRow(in.data() + r, n, out);
        }
    }
    else if (mode.rfind("deflate:", 0) == 0)
    {
        out = deflateBlock(in, atoi(mode.c_str() + 8));
    }
    else
        return 2;
    fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
