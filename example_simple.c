#include "fastpattern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

typedef struct {
    const char* name;
    const char* pattern_str;
    const char* description;
} game_pattern_t;

static game_pattern_t game_patterns[] = {
    {
        "UTIL_SayTextFilter", 
        "48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 49 8B D8",
        "Text filter utility function"
    },
    {
        "UTIL_SayText2Filter",
        "48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 41 0F B6 F8",
        "Text filter utility function v2"
    },
    {
        "CEntitySystem_AddEntityIOEvent",
        "48 89 5C 24 ?? 4C 89 4C 24 ?? 48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 49 8B F9",
        "Entity IO event system"
    },
    {
        "TriggerPush_Touch",
        "40 55 53 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 02 48 8B F9",
        "Trigger push touch handler"
    },
    {
        "SetGroundEntity",
        "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 54 48 83 EC ?? 44 8B 89",
        "Ground entity setter"
    },
    {
        "CCSPlayerController_SwitchTeam",
        "40 53 57 48 81 EC ?? ?? ?? ?? 48 8B D9 8B FA",
        "Player team switching"
    },
    {
        "CheckJumpButtonWater",
        "C8 42 EB ?? 4C 39 67 30",
        "Water jump button check"
    },
    {
        "UTIL_Remove",
        "48 85 C9 74 ?? 48 8B D1 48 8B 0D ?? ?? ?? ??",
        "Entity removal utility"
    }
};

#define NUM_GAME_PATTERNS (sizeof(game_patterns) / sizeof(game_patterns[0]))

static int parse_pattern_for_test(const char* pattern_str, uint8_t** pattern, uint8_t** mask, size_t* length) {
    size_t str_len = strlen(pattern_str);
    size_t max_bytes = (str_len + 1) / 3;
    
    *pattern = malloc(max_bytes);
    *mask = malloc(max_bytes);
    if (!*pattern || !*mask) {
        free(*pattern);
        free(*mask);
        return 0;
    }
    
    size_t byte_count = 0;
    const char* p = pattern_str;
    
    while (*p && byte_count < max_bytes) {
        while (*p == ' ') p++;
        if (!*p) break;
        
        if (*p == '?') {
            (*pattern)[byte_count] = 0x00;
            (*mask)[byte_count] = 0x00;
            p++;
            if (*p == '?') p++;
        } else {
            char hex_str[3] = {0};
            hex_str[0] = *p++;
            if (*p && *p != ' ') hex_str[1] = *p++;
            
            unsigned int byte_val;
            if (sscanf(hex_str, "%x", &byte_val) == 1) {
                (*pattern)[byte_count] = (uint8_t)byte_val;
                (*mask)[byte_count] = 0xFF;
            } else {
                free(*pattern);
                free(*mask);
                return 0;
            }
        }
        byte_count++;
    }
    
    *length = byte_count;
    return 1;
}

static uint8_t* load_server_dll(size_t* file_size) {
    FILE* file = fopen("server.dll", "rb");
    if (!file) {
        printf("Error: Could not open server.dll\n");
        return NULL;
    }
    
    fseek(file, 0, SEEK_END);
    *file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    uint8_t* data = malloc(*file_size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    
    size_t read_bytes = fread(data, 1, *file_size, file);
    fclose(file);
    
    if (read_bytes != *file_size) {
        free(data);
        return NULL;
    }
    
    return data;
}

static double get_time_ms() {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

int main() {
    printf("%s\n", fp_ver());
    printf("%s\n", fp_cpuinfo());
    printf("\n");
    
    size_t dll_size;
    uint8_t* dll_data = load_server_dll(&dll_size);
    if (!dll_data) {
        printf("Failed to load server.dll (%.2f MB expected)\n", 25.0);
        return 1;
    }
    
    printf("Loaded server.dll: %zu bytes (%.2f MB)\n", dll_size, dll_size / (1024.0 * 1024.0));
    printf("\nSearching for CS2 game patterns...\n");
    printf("==================================\n");
    
    double total_search_time = 0.0;
    int found_count = 0;
    
    for (size_t i = 0; i < NUM_GAME_PATTERNS; i++) {
        printf("\n%zu. %s\n", i + 1, game_patterns[i].name);
        printf("   Pattern: %s\n", game_patterns[i].pattern_str);
        
        const int iterations = 1000;
        size_t result1 = SIZE_MAX;
        
        uint8_t* pattern = NULL;
        uint8_t* mask = NULL;
        size_t pattern_len = 0;
        if (!parse_pattern_for_test(game_patterns[i].pattern_str, &pattern, &mask, &pattern_len)) {
            printf("   Failed to parse pattern\n");
            continue;
        }
        
        double start_time1 = get_time_ms();
        for (int iter = 0; iter < iterations; iter++) {
            result1 = fp_find(dll_data, dll_size, pattern, pattern_len, mask);
        }
        double end_time1 = get_time_ms();
        double time1 = (end_time1 - start_time1) / iterations;
        
        double start_time2 = get_time_ms();
        size_t result2 = SIZE_MAX;
        for (int iter = 0; iter < iterations; iter++) {
            result2 = fp_find_pattern(dll_data, dll_size, game_patterns[i].pattern_str);
        }
        double end_time2 = get_time_ms();
        double time2 = (end_time2 - start_time2) / iterations;
        
        if (result1 != SIZE_MAX) {
            printf("   Found at: 0x%08X\n", (unsigned int)result1);
            printf("   fp_find():         %.3f ms (1000 iterations)\n", time1);
            printf("   fp_find_pattern(): %.3f ms (1000 iterations)\n", time2);
            printf("   Performance ratio: %.1fx (fp_find_pattern is %.1fx %s)\n", 
                   time2 / time1, 
                   time2 > time1 ? time2 / time1 : time1 / time2,
                   time2 > time1 ? "slower" : "faster");
            found_count++;
        } else {
            printf("   Not found\n");
            printf("   fp_find():         %.3f ms (1000 iterations)\n", time1);
            printf("   fp_find_pattern(): %.3f ms (1000 iterations)\n", time2);
        }
        
        total_search_time += time2;
        
        free(pattern);
        free(mask);
    }
    
    printf("\n==================================\n");
    printf("Search Summary:\n");
    printf("  Patterns found: %d/%d\n", found_count, (int)NUM_GAME_PATTERNS);
    printf("  Total search time: %.3f ms\n", total_search_time);
    printf("  Average per pattern: %.3f ms\n", total_search_time / NUM_GAME_PATTERNS);
    printf("  File size: %.2f MB\n", dll_size / (1024.0 * 1024.0));
    
    free(dll_data);
    
#ifdef _WIN32
    system("pause");
#endif
    
    return 0;
}