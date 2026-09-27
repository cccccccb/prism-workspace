#include "prism/core/logging.hpp"
#include "prism/pack/package.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>

void PrintUsage()
{
    std::cout << "Prism Application Packaging Tool (prism-pack)\n"
              << "Usage:\n"
              << "  prism-pack pack <app_dir> <output.prismpkg> [bin_dir]\n"
              << "  prism-pack inspect <package.prismpkg>\n"
              << "  prism-pack unpack <package.prismpkg> <dest_dir>\n"
              << "  prism-pack extract <package.prismpkg> <file_name> <dest_file>\n";
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "pack") {
        if (argc < 4) {
            std::cerr << "Error: 'pack' requires <app_dir> and <output.prismpkg>\n";
            PrintUsage();
            return 1;
        }
        std::string src_dir = argv[2];
        std::string out_pkg = argv[3];
        std::string bin_dir = (argc >= 5) ? argv[4] : "";
        return prism::pack::PackageManager::PackDirectory(src_dir, out_pkg, bin_dir) ? 0 : 1;

    } else if (command == "inspect") {
        if (argc < 3) {
            std::cerr << "Error: 'inspect' requires <package.prismpkg>\n";
            return 1;
        }
        std::string pkg_path = argv[2];
        auto info = prism::pack::PackageManager::Inspect(pkg_path);
        if (!info) {
            std::cerr << "Failed to inspect package: " << pkg_path << "\n";
            return 1;
        }

        std::cout << "========================================================\n"
                  << "  Prism Application Package (.prismpkg) Inspection\n"
                  << "========================================================\n"
                  << "  App ID       : " << info->app_id << "\n"
                  << "  App Name     : " << info->app_name << "\n"
                  << "  Version      : " << info->version << "\n"
                  << "  Executable   : " << info->exec_entry << "\n"
                  << "  Total Size   : " << info->total_size << " bytes\n"
                  << "  Files Count  : " << info->entries.size() << "\n"
                  << "--------------------------------------------------------\n"
                  << "  Entry Name               Offset      Size (bytes)   Checksum\n"
                  << "--------------------------------------------------------\n";
        for (const auto &ent : info->entries) {
            std::cout << "  " << std::left << std::setw(24) << ent.file_name << std::right
                      << std::setw(8) << ent.file_offset << std::setw(14) << ent.file_size
                      << "   0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                      << ent.checksum << std::dec << std::setfill(' ') << "\n";
        }
        std::cout << "========================================================\n";
        return 0;

    } else if (command == "unpack") {
        if (argc < 4) {
            std::cerr << "Error: 'unpack' requires <package.prismpkg> and <dest_dir>\n";
            return 1;
        }
        return prism::pack::PackageManager::Unpack(argv[2], argv[3]) ? 0 : 1;

    } else if (command == "extract") {
        if (argc < 5) {
            std::cerr << "Error: 'extract' requires <package.prismpkg> <file_name> <dest_file>\n";
            return 1;
        }
        auto data = prism::pack::PackageManager::ExtractFile(argv[2], argv[3]);
        if (data.empty()) {
            std::cerr << "Error: file not found in package: " << argv[3] << "\n";
            return 1;
        }
        std::ofstream out(argv[4], std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), data.size());
        std::cout << "Extracted " << data.size() << " bytes to " << argv[4] << "\n";
        return 0;

    } else {
        PrintUsage();
        return 1;
    }
}
