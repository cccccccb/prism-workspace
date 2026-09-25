#pragma once

namespace prism::core {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

// printf-style logging. The implementation serializes complete messages.
void Log(LogLevel level, const char* tag, const char* format, ...);

} // namespace prism::core

#define PRISM_LOG_DEBUG(tag, ...) ::prism::core::Log(::prism::core::LogLevel::Debug, tag, __VA_ARGS__)
#define PRISM_LOG_INFO(tag, ...)  ::prism::core::Log(::prism::core::LogLevel::Info, tag, __VA_ARGS__)
#define PRISM_LOG_WARN(tag, ...)  ::prism::core::Log(::prism::core::LogLevel::Warn, tag, __VA_ARGS__)
#define PRISM_LOG_ERROR(tag, ...) ::prism::core::Log(::prism::core::LogLevel::Error, tag, __VA_ARGS__)
