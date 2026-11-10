// SCDA-XBERecomp's log macros, to stderr like the rest of xboxrecomp.
#pragma once
#include <atomic>
#include <cstdio>

#define LOG_AT_(tag, ...)                    \
    do {                                     \
        std::fprintf(stderr, "[SCDA] " tag); \
        std::fprintf(stderr, __VA_ARGS__);   \
        std::fputc('\n', stderr);            \
    } while (0)
#define LOGI(...) LOG_AT_("", __VA_ARGS__)
#define LOGR(...) LOG_AT_("", __VA_ARGS__)
#define LOGW(...) LOG_AT_("warning: ", __VA_ARGS__)
#define LOGE(...) LOG_AT_("error: ", __VA_ARGS__)
// the first n times this line runs
#define LOG_FIRST(n, ...)                                        \
    do {                                                         \
        static std::atomic<int> log_first_count_{0};             \
        if (log_first_count_.fetch_add(1) < (n)) LOGI(__VA_ARGS__); \
    } while (0)
