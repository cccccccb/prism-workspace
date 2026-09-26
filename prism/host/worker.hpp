#pragma once
#include <csignal>
#include <filesystem>
#include <string>
int RunWorker(int fd, const std::filesystem::path& apps, const std::string& socket,
              int parent_pid, const volatile std::sig_atomic_t& stopping);
