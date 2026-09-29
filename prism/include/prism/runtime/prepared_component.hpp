#pragma once

#include "prism/runtime/blueprint.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace prism::runtime {

struct ComponentSource {
    std::string component_id;
    std::string source_path;
    std::string source_version;
};

enum class LoadStage { Read, Syntax, Semantic, ResourceLink, Install, Cancelled };

struct LoadDiagnostic {
    LoadStage stage{LoadStage::Semantic};
    ComponentSource source;
    int line{0};
    std::string message;
};

class LoadFailure : public std::runtime_error {
public:
    explicit LoadFailure(LoadDiagnostic diagnostic);
    const LoadDiagnostic &Diagnostic() const noexcept;

private:
    LoadDiagnostic diagnostic_;
};

// This index addresses the immutable URI table, never a renderer resource ID.
struct ImageReference {
    std::size_t resource_key;
};

struct PreparedImage {
    std::string uri;
    int line{1};
};

using PreparedPropertyValue =
    std::variant<double, bool, std::string, contracts::Color, ImageReference>;

struct PreparedPropertyAssignment {
    DslProperty id;
    PreparedPropertyValue value;
};

struct PreparedNode {
    Kind kind{Kind::Box};
    std::vector<PreparedPropertyAssignment> properties;
    std::vector<PropertyBinding> bindings;
    std::vector<ThemeRef> theme_refs;
    std::vector<TransitionSpec> transitions;
    std::vector<StateRule> state_rules;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::vector<PreparedNode> children;
    int line{1};
    // Named mount wrapper. Only the layout compiler/composer assigns this;
    // ordinary visual DSL cannot invent a region or a live node identity.
    std::string region;
    bool region_mounted{};
};

using ResolveImage = std::function<contracts::ResourceId(std::string_view)>;

class PreparedComponent {
public:
    PreparedComponent(const PreparedComponent &) = default;
    PreparedComponent(PreparedComponent &&) noexcept = default;
    PreparedComponent &operator=(const PreparedComponent &) = default;
    PreparedComponent &operator=(PreparedComponent &&) noexcept = default;

    explicit operator bool() const noexcept;
    const ComponentSource &Source() const;
    const PreparedNode &Root() const;
    std::span<const PreparedImage> Images() const;
    std::size_t SourceBytes() const;
    std::size_t NodeCount() const;
    std::uint64_t RetainedBytes() const noexcept;
    PreparedComponent WithRetention(std::shared_ptr<const void> retention) const;

private:
    struct Data;
    explicit PreparedComponent(std::shared_ptr<const Data> data);
    const Data &GetData() const;
    std::shared_ptr<const Data> data_;
    std::shared_ptr<const void> retention_;

    friend PreparedComponent PrepareComponent(std::string_view, ComponentSource);
    friend struct PreparedComponentAccess;
};

// CPU-only, owning and immutable. Theme names remain references until installation.
PreparedComponent PrepareComponent(std::string_view source, ComponentSource source_info = {});

// Call on the resource owner's thread after preparation has succeeded completely.
Blueprint LinkComponent(const PreparedComponent &prepared, ResolveImage resolve_image = {});

} // namespace prism::runtime
