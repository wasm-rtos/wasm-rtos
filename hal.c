#if !defined(MICROWASM_BROWSER)

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#endif

#include "hal.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static int g_hal_initialized = 0;

#if defined(_WIN32)
static LARGE_INTEGER g_hal_start_counter;
static LARGE_INTEGER g_hal_counter_frequency;
#else
static struct timespec g_hal_start_time;
#endif

void hal_init(void)
{
#if defined(_WIN32)
    if (QueryPerformanceFrequency(&g_hal_counter_frequency) == 0 ||
        QueryPerformanceCounter(&g_hal_start_counter) == 0)
    {
        g_hal_counter_frequency.QuadPart = 0;
        g_hal_start_counter.QuadPart = 0;
        g_hal_initialized = 0;
        return;
    }
#else
    if (clock_gettime(CLOCK_MONOTONIC, &g_hal_start_time) != 0)
    {
        g_hal_start_time.tv_sec = 0;
        g_hal_start_time.tv_nsec = 0;
        g_hal_initialized = 0;
        return;
    }
#endif

    g_hal_initialized = 1;
}

void hal_shutdown(void)
{
    g_hal_initialized = 0;

#if defined(_WIN32)
    g_hal_start_counter.QuadPart = 0;
    g_hal_counter_frequency.QuadPart = 0;
#else
    g_hal_start_time.tv_sec = 0;
    g_hal_start_time.tv_nsec = 0;
#endif
}

uint32_t hal_get_time_ms(void)
{
    uint64_t elapsed_ms = 0U;

    if (!g_hal_initialized)
    {
        return 0U;
    }

#if defined(_WIN32)
    {
        LARGE_INTEGER current_counter;
        uint64_t elapsed_counts = 0U;

        if (g_hal_counter_frequency.QuadPart <= 0 ||
            QueryPerformanceCounter(&current_counter) == 0)
        {
            return 0U;
        }

        elapsed_counts = (uint64_t)(current_counter.QuadPart - g_hal_start_counter.QuadPart);
        elapsed_ms = (elapsed_counts * 1000U) / (uint64_t)g_hal_counter_frequency.QuadPart;
    }
#else
    {
        struct timespec current_time;
        time_t elapsed_seconds = 0;
        long elapsed_nanoseconds = 0;

        if (clock_gettime(CLOCK_MONOTONIC, &current_time) != 0)
        {
            return 0U;
        }

        elapsed_seconds = current_time.tv_sec - g_hal_start_time.tv_sec;
        elapsed_nanoseconds = current_time.tv_nsec - g_hal_start_time.tv_nsec;
        if (elapsed_nanoseconds < 0)
        {
            --elapsed_seconds;
            elapsed_nanoseconds += 1000000000L;
        }

        if (elapsed_seconds < 0)
        {
            return 0U;
        }

        elapsed_ms = ((uint64_t)elapsed_seconds * 1000U) +
            ((uint64_t)elapsed_nanoseconds / 1000000U);
    }
#endif

    return (uint32_t)elapsed_ms;
}

void hal_panic(const char* message)
{
    (void)message;

    for (;;)
    {
    }
}

#endif
