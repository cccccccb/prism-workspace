#pragma once

#include "prism/contracts/app_module.h"
#include <cstddef>
#include <cstdint>

namespace module_work_fixture {
enum class Mode : std::uint32_t { Echo, Empty, Overflow, RepeatedResult, Failure, Forbidden, Late };

struct Input {
    Mode mode{Mode::Echo};
    int entered{-1}, gate{-1}, exited{-1};
};

struct Record {
    std::uint64_t task{};
    std::uint32_t status{};
    std::int32_t error{};
    std::size_t size{};
    std::uint8_t bytes[128]{};
};
} // namespace module_work_fixture
