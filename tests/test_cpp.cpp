#include "fastpattern.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

int main()
{
    const std::uint8_t data[] = {0x00, 0xaa, 0x1f, 0xbb};
    const std::uint8_t pattern[] = {0xaa, 0x00, 0xbb};
    const std::uint8_t mask[] = {0xff, 0x00, 0xff};
    if (fp_find(data, sizeof(data), pattern, sizeof(pattern), mask) != 1 ||
        fp_find_pattern(data, sizeof(data), "AA ?F BB") != 1 ||
        fp_find_pattern(data, sizeof(data), "CC") != SIZE_MAX) {
        return 1;
    }
    fp_pattern *compiled = fp_compile("AA ?F BB");
    const std::size_t result = fp_find_compiled(data, sizeof(data), compiled);
    fp_free(compiled);
    fp_free(nullptr);
    if (result != 1) {
        return 1;
    }
    return fp_ver() && std::strlen(fp_ver()) &&
           fp_cpuinfo() && std::strlen(fp_cpuinfo()) ? 0 : 1;
}
