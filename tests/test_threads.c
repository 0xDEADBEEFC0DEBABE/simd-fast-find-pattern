#include "fastpattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

enum { THREAD_COUNT = 8, ITERATIONS = 2000, DATA_SIZE = 4099, PLANTED = 3001, SIG_COUNT = 4 };
static fp_set *shared_set;
static const char *const texts[SIG_COUNT] = {"F4 A7 C2 91", "F4 A? ?? 91", "C2 91 10", "AB CD EF"};
static const size_t expected[SIG_COUNT] = {PLANTED, PLANTED, PLANTED + 2, SIZE_MAX};

typedef struct {
    unsigned index;
    unsigned failures;
} thread_context;

#if defined(_WIN32)
static HANDLE start_event;
static HANDLE ready_event;
static volatile LONG ready_count;
#else
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static unsigned ready_count;
static int started;
#endif

static int scan_matches(const uint8_t *data, const fp_set *set, size_t *offsets)
{
    unsigned i;
    if (fp_scan(data, DATA_SIZE, set, offsets) != PLANTED) {
        return 0;
    }
    for (i = 0; i < SIG_COUNT; ++i) {
        if (offsets[i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static void run_worker(thread_context *context)
{
    static uint8_t buffers[THREAD_COUNT][DATA_SIZE];
    uint8_t *data = buffers[context->index];
    const uint8_t pattern[] = {0xf4, 0xa7, 0xc2, 0x91};
    const uint8_t mask[] = {0xff, 0xf0, 0, 0xff};
    const fp_sig exact = {pattern, NULL, sizeof(pattern)};
    const fp_sig masked = {pattern, mask, sizeof(pattern)};
    size_t offsets[SIG_COUNT];
    const char *cpu_info;
    char initial_cpu_info[512];
    fp_set *set, *single;
    unsigned iteration;
    memset(data, 0x10, DATA_SIZE);
    memcpy(data + PLANTED, pattern, sizeof(pattern));
#if defined(_WIN32)
    if (InterlockedIncrement(&ready_count) == THREAD_COUNT) {
        SetEvent(ready_event);
    }
    if (WaitForSingleObject(start_event, INFINITE) != WAIT_OBJECT_0) {
        ++context->failures;
        return;
    }
#else
    if (pthread_mutex_lock(&start_mutex) != 0) {
        ++context->failures;
        return;
    }
    ++ready_count;
    pthread_cond_broadcast(&start_condition);
    while (!started) {
        pthread_cond_wait(&start_condition, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
#endif
    /* This executable intentionally makes no library calls before this point,
     * so metadata and search callers race to initialize dispatch. */
    if (context->index % 2) {
        fp_set *first = fp_set_from_bytes(&exact, 1, NULL);
        if (!first || fp_scan(data, DATA_SIZE, first, NULL) != PLANTED) {
            ++context->failures;
        }
        fp_set_free(first);
    }
    cpu_info = fp_cpuinfo();
    if (!cpu_info || !cpu_info[0] || strlen(cpu_info) >= sizeof(initial_cpu_info)) {
        ++context->failures;
        return;
    }
    strcpy(initial_cpu_info, cpu_info);
    set = shared_set ? shared_set : fp_set_from_text(texts, SIG_COUNT, NULL);
    single = fp_set_from_bytes(&masked, 1, NULL);
    if (!set || !single) {
        ++context->failures;
    }
    for (iteration = 0; set && single && iteration < ITERATIONS; ++iteration) {
        if (!scan_matches(data, set, offsets) || fp_scan(data, DATA_SIZE, single, NULL) != PLANTED ||
            fp_scan(data, DATA_SIZE, set, NULL) != PLANTED ||
            strcmp(initial_cpu_info, fp_cpuinfo()) != 0 || !fp_ver() || !fp_ver()[0]) {
            ++context->failures;
            break;
        }
    }
    if (!shared_set) {
        fp_set_free(set);
    }
    fp_set_free(single);
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID argument)
{
    run_worker((thread_context *)argument);
    return 0;
}
#else
static void *worker_entry(void *argument)
{
    run_worker((thread_context *)argument);
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    thread_context contexts[THREAD_COUNT];
    unsigned i;
    if (argc == 2 && strcmp(argv[1], "--shared-compiled") == 0) {
        shared_set = fp_set_from_text(texts, SIG_COUNT, NULL);
        if (!shared_set) {
            fputs("Shared set compilation failed\n", stderr);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        return EXIT_FAILURE;
    }
#if defined(_WIN32)
    HANDLE threads[THREAD_COUNT];
    start_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!start_event || !ready_event) {
        fputs("CreateEvent failed\n", stderr);
        return EXIT_FAILURE;
    }
#else
    pthread_t threads[THREAD_COUNT];
#endif
    for (i = 0; i < THREAD_COUNT; ++i) {
        contexts[i].index = i;
        contexts[i].failures = 0;
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, worker_entry, &contexts[i], 0, NULL);
        if (!threads[i]) {
            fputs("CreateThread failed\n", stderr);
            return EXIT_FAILURE;
        }
#else
        if (pthread_create(&threads[i], NULL, worker_entry, &contexts[i]) != 0) {
            fputs("pthread_create failed\n", stderr);
            return EXIT_FAILURE;
        }
#endif
    }
#if defined(_WIN32)
    if (WaitForSingleObject(ready_event, INFINITE) != WAIT_OBJECT_0 || !SetEvent(start_event) ||
        WaitForMultipleObjects(THREAD_COUNT, threads, TRUE, INFINITE) != WAIT_OBJECT_0) {
        fputs("Thread synchronization failed\n", stderr);
        return EXIT_FAILURE;
    }
    for (i = 0; i < THREAD_COUNT; ++i) {
        CloseHandle(threads[i]);
    }
    CloseHandle(start_event);
    CloseHandle(ready_event);
#else
    pthread_mutex_lock(&start_mutex);
    while (ready_count != THREAD_COUNT) {
        pthread_cond_wait(&start_condition, &start_mutex);
    }
    started = 1;
    pthread_cond_broadcast(&start_condition);
    pthread_mutex_unlock(&start_mutex);
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            fputs("pthread_join failed\n", stderr);
            return EXIT_FAILURE;
        }
    }
    pthread_cond_destroy(&start_condition);
    pthread_mutex_destroy(&start_mutex);
#endif
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (contexts[i].failures) {
            fprintf(stderr, "Thread %u failed\n", i);
            return EXIT_FAILURE;
        }
    }
    printf("Passed %s concurrency test (%u threads, %u iterations each)\n",
           shared_set ? "shared set" : "cold-start",
           (unsigned)THREAD_COUNT, (unsigned)ITERATIONS);
    fp_set_free(shared_set);
    return EXIT_SUCCESS;
}
