#include "prism/core/logging.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace prism::core {

void Log(LogLevel level, const char *tag, const char *format, ...)
{
    static std::mutex output_mutex;
    const char *label = "INFO";
    switch (level) {
    case LogLevel::Debug:
        label = "DEBUG";
        break;
    case LogLevel::Info:
        label = "INFO";
        break;
    case LogLevel::Warn:
        label = "WARN";
        break;
    case LogLevel::Error:
        label = "ERROR";
        break;
    }

    std::lock_guard<std::mutex> lock(output_mutex);
    std::fprintf(stderr, "[%s] [%s] ", label, tag ? tag : "PRISM");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

} // namespace prism::core
