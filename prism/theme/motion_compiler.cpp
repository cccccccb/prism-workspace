#include "prism/theme/motion_compiler.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <utility>

namespace prism::theme {
namespace {
[[noreturn]] void Error(int line, const std::string &message)
{
    throw std::invalid_argument("Motion DSL line " + std::to_string(line) + ": " + message);
}

struct Args {
    const runtime::SyntaxNode &node;
    std::map<std::string, const runtime::SyntaxValue *> values;

    Args(const runtime::SyntaxNode &source, std::initializer_list<std::string_view> allowed,
         bool children = false)
        : node(source)
    {
        if (!node.modifiers.empty() || (!children && !node.children.empty())) {
            Error(node.line, "unexpected children or modifiers");
        }
        for (const auto &arg : node.arguments) {
            const auto key = arg.name.empty() ? "$name" : arg.name;
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end() ||
                !values.emplace(key, &arg.value).second) {
                Error(arg.line, "unknown or duplicate argument " + key);
            }
        }
        if (values.size() != allowed.size()) {
            Error(node.line, "missing required argument");
        }
    }

    std::string Text(const char *key) const
    {
        const auto *value = std::get_if<std::string>(&values.at(key)->data);
        if (!value || !contracts::ValidMotionName(*value)) {
            Error(node.line, std::string("invalid name: ") + key);
        }
        return *value;
    }

    unsigned Number(const char *key, unsigned limit) const
    {
        const auto *value = std::get_if<double>(&values.at(key)->data);
        if (!value || !std::isfinite(*value) || *value < 0 || *value > limit ||
            std::trunc(*value) != *value) {
            Error(node.line, std::string("invalid integer: ") + key);
        }
        return static_cast<unsigned>(*value);
    }
};

contracts::MotionEasing Easing(const Args &args)
{
    const auto name = args.Text("easing");
    const std::string_view names[]{"linear", "easeInCubic", "easeOutCubic", "easeInOutCubic"};
    const auto it = std::find(std::begin(names), std::end(names), name);
    if (it == std::end(names)) {
        Error(args.node.line, "unknown easing");
    }
    return static_cast<contracts::MotionEasing>(it - std::begin(names));
}
} // namespace

contracts::MotionSet CompileMotion(std::string_view source)
{
    if (source.empty() || source.size() > 65536 || source.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Invalid Motion source size or NUL");
    }
    const auto root = runtime::ParseSyntax(source);
    if (root.name != "MotionSet" || root.children.size() > 128) {
        Error(root.line, "expected bounded MotionSet");
    }
    const Args header(root, {"$name", "version"}, true);
    if (header.Number("version", 1) != 1) {
        Error(root.line, "unsupported MotionSet version");
    }
    contracts::MotionSet result{header.Text("$name"), {}};
    std::map<std::string, contracts::MotionTransition> timings;
    for (const auto &node : root.children) {
        if (node.name == "Timing") {
            const Args args(node, {"$name", "durationMs", "easing"});
            const auto name = args.Text("$name");
            if (timings.size() >= 64 ||
                !timings
                     .emplace(name,
                              contracts::MotionTransition{name, args.Number("durationMs", 10000),
                                                          Easing(args)})
                     .second) {
                Error(node.line, "duplicate or excessive timings");
            }
        } else if (node.name != "Transition") {
            Error(node.line, "unknown motion component");
        }
    }
    for (const auto &node : root.children) {
        if (node.name == "Transition") {
            const Args args(node, {"$name", "timing"});
            const auto found = timings.find(args.Text("timing"));
            if (found == timings.end()) {
                Error(node.line, "unknown timing reference");
            }
            auto entry = found->second;
            entry.name = args.Text("$name");
            result.transitions.push_back(std::move(entry));
        }
    }
    contracts::ValidateMotion(result);
    return result;
}

contracts::MotionSet LoadMotion(const std::filesystem::path &root, std::string_view id)
{
    if (!contracts::ValidMotionName(id) || id.find('.') != std::string_view::npos) {
        throw std::invalid_argument("Invalid motion package ID");
    }
    const auto base = std::filesystem::canonical(root);
    const auto file = std::filesystem::canonical(base / id / "motion.prism");
    const auto relative = file.lexically_relative(base);
    if (relative.empty() || *relative.begin() == "..") {
        throw std::invalid_argument("Motion package escapes its root");
    }
    const auto size = std::filesystem::file_size(file);
    if (!size || size > 65536) {
        throw std::invalid_argument("Motion package exceeds size limit");
    }
    std::ifstream input(file, std::ios::binary);
    std::string source(size, '\0');
    if (!input.read(source.data(), source.size())) {
        throw std::invalid_argument("Cannot read motion package");
    }
    auto result = CompileMotion(source);
    if (result.id != id) {
        throw std::invalid_argument("Motion package ID does not match directory");
    }
    return result;
}
} // namespace prism::theme
