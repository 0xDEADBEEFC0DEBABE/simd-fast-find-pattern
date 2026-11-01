#include "fastpattern.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

int main()
{
    const std::uint8_t data[] = {0x00, 0xaa, 0x1f, 0xbb};
    const std::uint8_t pattern[] = {0xaa, 0x00, 0xbb};
    const std::uint8_t mask[] = {0xff, 0x00, 0xff};
    const char *const texts[] = {"AA ?F BB", "CC"};
    const fp_sig sig = {pattern, mask, sizeof(pattern)};
    std::size_t offsets[2] = {0, 0};
    fp_error error;
    fp_set *bytes = fp_set_from_bytes(&sig, 1, &error);
    fp_set *text = fp_set_from_text(texts, 2, &error);
    const bool ok = bytes && text && error.status == FP_OK && fp_set_count(text) == 2 &&
                    fp_scan(data, sizeof(data), bytes, nullptr) == 1 &&
                    fp_scan(data, sizeof(data), text, offsets) == 1 &&
                    offsets[0] == 1 && offsets[1] == SIZE_MAX;
    fp_set_free(bytes);
    fp_set_free(text);
    fp_set_free(nullptr);
    if (!ok) {
        return 1;
    }
    return fp_ver() && std::strlen(fp_ver()) &&
           fp_cpuinfo() && std::strlen(fp_cpuinfo()) ? 0 : 1;
}
