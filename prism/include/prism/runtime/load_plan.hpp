#pragma once

#include "prism/runtime/prepared_component.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::runtime {

inline constexpr std::size_t kMaxLoadComponents = 128;
inline constexpr std::size_t kMaxLoadFileBytes = 1024 * 1024;
inline constexpr std::size_t kMaxLoadSourceBytes = 8 * 1024 * 1024;

enum class LoadPhase { Critical, Deferred };
enum class LoadBindingType { String, Number, Boolean, Color };

struct LoadBinding {
    std::string name;
    LoadBindingType type{LoadBindingType::String};
    PropertyValue initial;
    int line{1};
};

struct LoadUnit {
    std::string id;
    // Lexically validated package-root-relative path. The read boundary must
    // additionally reject symlink escape, nonregular files and size changes.
    std::filesystem::path source_path;
    LoadPhase phase{LoadPhase::Critical};
    std::vector<std::string> after;
    int line{1};
};

struct LoadPlan {
    std::uint32_t version{1};
    bool legacy{true};
    ComponentSource source;
    std::filesystem::path package_root;
    std::filesystem::path layout_path;
    std::vector<LoadUnit> components;
    std::vector<LoadBinding> bindings;
    // Entry source only. The scheduler counts every actual layout/component
    // read, including deferred results, once against the 8 MiB aggregate.
    std::size_t source_bytes{};
};

struct PreparedSlot {
    std::string component;
    // Child indices in Body().Root(). Slot wrappers survive composition.
    std::vector<std::size_t> node_path;
    int line{1};
};

class PreparedLayout {
public:
    PreparedLayout(const PreparedLayout &) = default;
    PreparedLayout(PreparedLayout &&) noexcept = default;
    PreparedLayout &operator=(const PreparedLayout &) = default;
    PreparedLayout &operator=(PreparedLayout &&) noexcept = default;

    static PreparedLayout Legacy();
    bool IsLegacy() const;
    const PreparedComponent &Body() const;
    std::span<const PreparedSlot> Slots() const;
    std::size_t RetainedBytes() const;
    // Keeps scheduler ownership alive without modifying the shared template.
    PreparedLayout WithRetention(std::shared_ptr<const void>) const;

private:
    struct Data;
    explicit PreparedLayout(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> data_;
    std::shared_ptr<const void> retention_;
    friend PreparedLayout PrepareLayout(std::string_view, const LoadPlan &, ComponentSource);
};

struct PreparedUnit {
    std::string id;
    PreparedComponent prepared;
};

// Syntax and semantic compilation only: no filesystem reads, Scene, theme,
// platform handles or resource registration. Ordinary visual input is legacy.
LoadPlan CompileLoadPlan(std::string_view source, ComponentSource metadata,
                         std::filesystem::path package_root);
// Header recognition never parses the visual body. Legacy preparation can run
// through the same compiler adapter exactly once before creating its plan.
bool IsInterfaceSource(std::string_view source, ComponentSource metadata = {});
// Metadata identifies the read request; cached IR may carry a different origin.
LoadPlan CompileLegacyLoadPlan(const PreparedComponent &, ComponentSource metadata,
                               std::filesystem::path package_root);
const LoadUnit *FindLoadUnit(const LoadPlan &, std::string_view id);
void ValidatePreparedUnit(const LoadPlan &, std::string_view id, const PreparedComponent &);

// Slot is accepted only here; ordinary PrepareComponent still rejects it.
PreparedLayout PrepareLayout(std::string_view source, const LoadPlan &,
                             ComponentSource metadata = {});

// Requires every critical unit exactly once. Deferred results may be supplied
// for validation but their placeholders remain inert. Declaration order in the
// layout determines painting, independently of worker completion order.
// Known/potential explicit blur regions are checked here; material expansion
// uses the current theme and is checked by the owner-thread Scene installation.
PreparedComponent ComposeCritical(const LoadPlan &, const PreparedLayout &,
                                  std::span<const PreparedUnit> units);

} // namespace prism::runtime
