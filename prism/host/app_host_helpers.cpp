#include "app_host_p.hpp"

namespace prism::sdk::host_detail {
std::string ReadUi(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage,
                                    "Cannot read package UI");
    }
    std::string source;
    char bytes[4096];
    while (input.read(bytes, sizeof(bytes)) || input.gcount()) {
        source.append(bytes, input.gcount());
        if (source.size() > 1024 * 1024) {
            throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "UI exceeds 1 MiB");
        }
    }
    if (input.bad() || source.empty()) {
        throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "Invalid package UI");
    }
    return source;
}

runtime::PreparedComponent PrepareUi(const std::filesystem::path &file, std::string component)
{
    return runtime::PrepareComponent(ReadUi(file), {std::move(component), file.string(), {}});
}

std::string UiFailureDetail(const runtime::LoadDiagnostic &diagnostic)
{
    auto location = diagnostic.source.source_path.empty() ? diagnostic.source.component_id
                                                          : diagnostic.source.source_path;
    if (diagnostic.line > 0) {
        location += (location.empty() ? "DSL line " : ":") + std::to_string(diagnostic.line);
    }
    return location.empty() ? diagnostic.message : location + ": " + diagnostic.message;
}

bool CallerInputReady(std::span<const pollfd> descriptors)
{
    for (const auto &fd : descriptors) {
        if (fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            return true;
        }
    }
    return false;
}

void CollectBindings(const runtime::PreparedNode &node, std::set<std::string, std::less<>> &names)
{
    for (const auto &binding : node.bindings) {
        names.insert(binding.name);
    }
    for (const auto &child : node.children) {
        CollectBindings(child, names);
    }
}

bool MatchesBinding(runtime::LoadBindingType type, const runtime::PropertyValue &value)
{
    switch (type) {
    case runtime::LoadBindingType::String:
        return std::holds_alternative<std::string>(value);
    case runtime::LoadBindingType::Number:
        return std::holds_alternative<double>(value) && std::isfinite(std::get<double>(value));
    case runtime::LoadBindingType::Boolean:
        return std::holds_alternative<bool>(value);
    case runtime::LoadBindingType::Color:
        return runtime::ValidPropertyValue(runtime::DslProperty::Foreground, value);
    }
    return false;
}
} // namespace prism::sdk::host_detail
