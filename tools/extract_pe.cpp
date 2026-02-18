// Simple tool to extract the decompressed PE image from an XEX2 file
// using the XenonRecomp's XenonUtils library.
// Compile: cl /EHsc /I XenonRecomp\XenonUtils /I XenonRecomp\thirdparty\TinySHA1
//          /I XenonRecomp\thirdparty\tiny-AES-c extract_pe.cpp XenonRecomp\build\XenonUtils\XenonUtils.lib

#include <cstdio>
#include <file.h>
#include <image.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: extract_pe <input.xex> <output.bin>\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("ERROR: Failed to load %s\n", argv[1]);
        return 1;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        printf("ERROR: Failed to parse XEX image\n");
        return 1;
    }

    printf("Base: 0x%llX\n", (unsigned long long)image.base);
    printf("Size: 0x%X\n", image.size);
    printf("Entry: 0x%llX\n", (unsigned long long)image.entry_point);

    FILE* out = fopen(argv[2], "wb");
    if (!out)
    {
        printf("ERROR: Failed to open output file %s\n", argv[2]);
        return 1;
    }

    fwrite(image.data.get(), 1, image.size, out);
    fclose(out);

    printf("Wrote %u bytes to %s\n", image.size, argv[2]);
    return 0;
}
