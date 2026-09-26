#pragma once
#include "prism/contracts/launch.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace prism::launch {
class LaunchFailure : public std::runtime_error {
public:
    LaunchFailure(contracts::LaunchError code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}
    contracts::LaunchError Code() const { return code_; }
private:
    contracts::LaunchError code_;
};
} // namespace prism::launch
