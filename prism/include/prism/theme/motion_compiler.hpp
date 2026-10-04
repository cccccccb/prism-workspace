#pragma once
#include "prism/contracts/motion.hpp"
#include <filesystem>
#include <string_view>

namespace prism::theme {
contracts::MotionSet CompileMotion(std::string_view source);
contracts::MotionSet LoadMotion(const std::filesystem::path &root, std::string_view id);
} // namespace prism::theme
