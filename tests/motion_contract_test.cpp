#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <unistd.h>
using namespace prism;

namespace {
void Reject(const std::function<void()> &call)
{
    bool rejected{};
    try {
        call();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}

void CheckSource()
{
    const auto set = theme::CompileMotion(R"(
        MotionSet("fixture", version:1) {
            Transition("panel.visibility", timing:"later")
            Timing("later", durationMs:120, easing:"easeOutCubic")
        })");
    assert(set.id == "fixture" && set.transitions.size() == 1);
    assert(contracts::FindMotion(set, "panel.visibility")->duration_ms == 120);
    assert(!contracts::FindMotion(set, "missing"));
    for (const auto *body :
         {"Timing(\"a\",durationMs:0.5,easing:\"linear\")",
          "Timing(\"a\",durationMs:10001,easing:\"linear\")",
          "Timing(\"a\",durationMs:1,easing:\"unknown\")",
          "Timing(\"a\",durationMs:1,easing:\"linear\",extra:1)",
          "Transition(\"x\",timing:\"absent\")",
          "Transition(\"x\",timing:\"base\") Transition(\"x\",timing:\"base\")", "Unknown()"}) {
        Reject([&] {
            theme::CompileMotion(std::string("MotionSet(\"test\",version:1) {") +
                                 "Timing(\"base\",durationMs:0,easing:\"linear\") " + body + "}");
        });
    }
    Reject([] { theme::CompileMotion("MotionSet(\"x\",version:2){}"); });
    Reject([] { theme::CompileMotion(std::string(65537, ' ')); });
}

void CheckPackages()
{
    const auto root = std::filesystem::path(PRISM_SOURCE_ROOT) / "resources";
    for (const auto *id : {"prism", "subtle", "instant"}) {
        const auto motion = theme::LoadMotion(root / "motions", id);
        assert(contracts::FindMotion(motion, "group.geometry"));
        auto snapshot = theme::LoadTheme(root / "themes", "glass", 1);
        std::ifstream input(root / "themes/glass/theme.prism");
        std::string source((std::istreambuf_iterator<char>(input)), {});
        const std::string reference = "motion: \"prism\"";
        const auto position = source.find(reference);
        assert(position != std::string::npos);
        source.replace(position, reference.size(), "motion: \"" + std::string(id) + "\"");
        const auto compiled = theme::CompileTheme(source, 1, "dark", &motion);
        assert(compiled.motion == motion);
        Reject([&] { theme::CompileTheme(source, 1); });
        snapshot.motion = motion;
        const auto bytes = contracts::EncodeTheme(snapshot);
        assert(contracts::DecodeTheme(bytes) == snapshot);
        for (std::size_t i = bytes.size() - 10; i < bytes.size(); ++i) {
            Reject([&] { contracts::DecodeTheme(std::span(bytes).first(i)); });
        }
        auto invalid = snapshot;
        invalid.motion.transitions.push_back(invalid.motion.transitions.front());
        Reject([&] { contracts::EncodeTheme(invalid); });
        invalid = snapshot;
        invalid.schema_version = 2;
        Reject([&] { contracts::EncodeTheme(invalid); });
    }
    Reject([&] { theme::LoadMotion(root / "motions", "../prism"); });
    Reject([&] { theme::LoadMotion(root / "motions", "absent"); });

    char directory[] = "/tmp/prism-motion-package.XXXXXX";
    assert(mkdtemp(directory));
    const std::filesystem::path temp(directory);
    std::filesystem::create_directory_symlink(root / "motions/prism", temp / "escape");
    Reject([&] { theme::LoadMotion(temp, "escape"); });
    std::filesystem::create_directories(temp / "mismatch");
    std::filesystem::copy_file(root / "motions/prism/motion.prism", temp / "mismatch/motion.prism");
    Reject([&] { theme::LoadMotion(temp, "mismatch"); });
    std::filesystem::remove_all(temp);
}
} // namespace

int main()
{
    CheckSource();
    CheckPackages();
}
