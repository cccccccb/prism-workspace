#include "prism/runtime/prepared_component.hpp"
#include "prepared_component_p.hpp"
#include <utility>

namespace prism::runtime {
namespace {
std::string DiagnosticText(const LoadDiagnostic &diagnostic)
{
    if (diagnostic.line > 0) {
        return "DSL line " + std::to_string(diagnostic.line) + ": " + diagnostic.message;
    }
    return diagnostic.message;
}

[[noreturn]] void LinkError(const ComponentSource &source, int line, std::string message)
{
    throw LoadFailure({LoadStage::ResourceLink, source, line, std::move(message)});
}

PropertyValue LinkValue(const PreparedPropertyValue &value,
                        const std::vector<contracts::ResourceId> &images)
{
    if (const auto *number = std::get_if<double>(&value)) {
        return *number;
    }
    if (const auto *boolean = std::get_if<bool>(&value)) {
        return *boolean;
    }
    if (const auto *text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto *color = std::get_if<contracts::Color>(&value)) {
        return *color;
    }
    return images.at(std::get<ImageReference>(value).resource_key);
}

Blueprint LinkNode(const PreparedNode &node, const std::vector<contracts::ResourceId> &images)
{
    Blueprint result;
    result.kind = node.kind;
    result.allowed_properties = node.allowed_properties;
    result.bindings = node.bindings;
    result.theme_refs = node.theme_refs;
    result.properties.reserve(node.properties.size());
    result.children.reserve(node.children.size());

    for (const auto &property : node.properties) {
        result.properties.push_back({property.id, LinkValue(property.value, images)});
    }
    for (const auto &child : node.children) {
        result.children.push_back(LinkNode(child, images));
    }
    return result;
}
} // namespace

LoadFailure::LoadFailure(LoadDiagnostic diagnostic)
    : std::runtime_error(DiagnosticText(diagnostic)), diagnostic_(std::move(diagnostic))
{
}

const LoadDiagnostic &LoadFailure::Diagnostic() const noexcept
{
    return diagnostic_;
}

PreparedComponent::PreparedComponent(std::shared_ptr<const Data> data) : data_(std::move(data))
{
}

PreparedComponent::operator bool() const noexcept
{
    return static_cast<bool>(data_);
}

const PreparedComponent::Data &PreparedComponent::GetData() const
{
    if (!data_) {
        throw std::logic_error("PreparedComponent has no immutable data");
    }
    return *data_;
}

const ComponentSource &PreparedComponent::Source() const
{
    return GetData().source;
}

const PreparedNode &PreparedComponent::Root() const
{
    return GetData().root;
}

std::span<const PreparedImage> PreparedComponent::Images() const
{
    return GetData().images;
}

std::size_t PreparedComponent::SourceBytes() const
{
    return GetData().source_bytes;
}

std::size_t PreparedComponent::NodeCount() const
{
    return GetData().node_count;
}

Blueprint LinkComponent(const PreparedComponent &prepared, ResolveImage resolve_image)
{
    if (!prepared) {
        LinkError({}, 0, "Cannot link an invalid PreparedComponent");
    }
    const auto &source = prepared.Source();
    const auto resources = prepared.Images();
    if (!resources.empty() && !resolve_image) {
        LinkError(source, resources.front().line, "Image requires a resource resolver and URI");
    }

    std::vector<contracts::ResourceId> images(resources.size());
    for (std::size_t i = 0; i < resources.size(); ++i) {
        const auto &resource = resources[i];
        try {
            images[i] = resolve_image(resource.uri);
        } catch (const std::exception &error) {
            LinkError(source, resource.line,
                      "Image resource request failed: " + resource.uri + ": " + error.what());
        } catch (...) {
            LinkError(source, resource.line,
                      "Image resource request failed: " + resource.uri + ": unknown exception");
        }
        if (!images[i]) {
            LinkError(source, resource.line, "Image resource request failed: " + resource.uri);
        }
    }
    return LinkNode(prepared.Root(), images);
}

} // namespace prism::runtime
