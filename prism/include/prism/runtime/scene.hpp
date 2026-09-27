#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/runtime/blueprint.hpp"
#include "prism/runtime/ui_install.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace prism::runtime {
struct RenderTree;

struct Style {
    bool visible{true};
    double width{0};  // automatic: intrinsic leaves, remaining-space containers
    double height{0}; // automatic; Card fills containers and measures text
    double padding{0};
    double spacing{0};
    double radius{0};
    contracts::Color background{0, 0, 0, 0};
    contracts::Color foreground{255, 255, 255, 255};
    double font_size{16};
    bool clip{false};
    std::string align{"stretch"};
    std::string justify{"start"};
    std::string anchor{"fill"};
    std::string overflow{"visible"};
    double flex{0};
    double inset{0};
    double padding_x{-1}, padding_y{-1};
    double border_width{0};
    contracts::Color border_color{255, 255, 255, 0};
    double shadow_blur{0}, shadow_y{0};
    contracts::Color shadow_color{0, 0, 0, 0};
    double inner_shadow_blur{0};
    double inner_shadow_y{0};
    contracts::Color inner_shadow_color{0, 0, 0, 0};
    double backdrop_blur{0};
    contracts::ImageFit image_fit{contracts::ImageFit::Fill};
    std::string material;
    std::string input_shape{"visible"};
};

// Text shaping belongs to the client. The renderer only receives glyph IDs.
struct ShapedText {
    std::vector<contracts::GlyphPlacement> glyphs;
    double width{0};
    double height{0};
};

using ShapeText = std::function<ShapedText(std::string_view, double)>;

// Per-scene work; isolated theme validation candidates have their own counters.
struct SceneRenderStats {
    std::uint64_t build_calls{}, builds{}, layouts{};
    bool operator==(const SceneRenderStats &) const = default;
};

class SceneConstruction;
class Scene;

class Scene {
public:
    explicit Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font = {},
                   std::optional<contracts::ThemeSnapshot> theme = {});
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    bool SetSlot(std::string_view name, std::string value);
    bool AcceptsBinding(std::string_view name, const PropertyValue &value) const;
    bool SetBinding(std::string_view name, PropertyValue value);
    bool SetProperty(contracts::NodeId id, DslProperty property, PropertyValue value);
    // Validates a detached candidate before changing the retained scene.
    bool ApplyTheme(const contracts::ThemeSnapshot &, std::string *diagnostic = nullptr);

    // Binding projection and validation are atomic; unknown unmounted keys are ignored.
    bool Preflight(const BindingValues &, std::string *diagnostic = nullptr);
    // Throwaway candidates only: avoids duplicating budgeted node construction.
    // A failed candidate may be mutated and must not be installed.
    bool PrepareDetached(const BindingValues &, std::string *diagnostic = nullptr);
    bool MountRegions(std::span<const RegionUpdate>, const BindingValues &,
                      std::string *diagnostic = nullptr);
    Blueprint RegionBlueprint(std::span<const RegionUpdate>) const;
    // Success consumes the detached candidate; failure never mutates the live scene.
    bool MountRegions(std::span<const RegionUpdate>, const BindingValues &, Scene &candidate,
                      std::uint64_t expected_revision, std::string *diagnostic = nullptr);
    bool RegionMounted(std::string_view region) const;

    std::uint64_t TransactionRevision() const noexcept
    {
        return transaction_revision_;
    }

    contracts::NodeId RegionId(std::string_view region) const;

    std::uint64_t ThemeGeneration() const
    {
        return theme_ ? theme_->generation : 0;
    }

    bool SetViewport(contracts::LogicalSize size);
    bool SetBackground(contracts::NodeId id, contracts::Color color);
    bool ImageReady(contracts::ResourceId image, contracts::LogicalSize intrinsic_size);
    std::optional<contracts::DisplayList> Build(contracts::WindowId window);
    std::optional<std::string> ActionAt(contracts::LogicalPoint point) const;

    Dirty PendingDirty() const
    {
        return dirty_;
    }

    // Content invalidation is independent of display-list builds or surface
    // commits. A failed presentation can retry an already-built list.
    std::uint64_t PixelsRevision() const
    {
        return pixels_revision_;
    }

    void AcknowledgeComposite();

    std::uint64_t Generation() const
    {
        return generation_;
    }

    SceneRenderStats GetRenderStats() const
    {
        return {build_calls_, generation_, layout_count_};
    }

    contracts::NodeId RootId() const;
    contracts::LogicalRect Bounds(contracts::NodeId id) const;
    bool IsVisible(contracts::NodeId id) const;
    std::vector<contracts::SurfaceEffectRegion> SurfaceEffects() const;
    const std::vector<contracts::SurfaceInputRegion> &InputRegions() const;
    bool SetPointer(contracts::LogicalPoint point);
    bool FocusNext();
    std::optional<std::string> FocusedAction() const;

private:
    friend class SceneConstruction;
    struct Node;

    struct EmptyConstruction {};

    Scene(EmptyConstruction, ShapeText, contracts::ResourceId,
          std::optional<contracts::ThemeSnapshot>);
    std::unique_ptr<Node> MakeNode(Blueprint blueprint, std::size_t depth = 1);
    std::unique_ptr<Node> MakeShallowNode(Blueprint blueprint, std::size_t depth);
    void ValidateCandidate(Scene &, const BindingValues &) const;
    void ValidateRetainedValues(const std::vector<std::pair<Node *, Node *>> &,
                                const BindingValues &) const;
    void CopyResources(const Node &, Node &) const;
    void CollectPairs(Node &, Node &, std::vector<std::pair<Node *, Node *>> &) const;
    void CommitValues(const std::vector<std::pair<Node *, Node *>> &) noexcept;
    void CollectNodes(Node &, std::vector<Node *> &) const;
    void CollectMountPairs(Node &, Node &, const std::set<std::string> &,
                           std::vector<std::pair<Node *, Node *>> &) const;
    Blueprint CurrentBlueprint(const Node &) const;
    Node *Find(contracts::NodeId id) const;
    bool IsVisible(const Node &) const;
    void ApplyCachedProperty(Node &node, DslProperty property, const PropertyValue &value);
    PropertyValue CurrentProperty(const Node &node, DslProperty property) const;
    std::optional<std::string> Hit(const Node &node, contracts::LogicalPoint point) const;
    void CollectSurfaceEffects(const Node &, std::vector<contracts::SurfaceInputRegion> &,
                               std::vector<contracts::SurfaceEffectRegion> &) const;
    void AddInputRegion(contracts::SurfaceInputRegion,
                        const std::vector<contracts::SurfaceInputRegion> &) const;
    void CollectInputRegions(const Node &, std::vector<contracts::SurfaceInputRegion> &) const;
    void Invalidate(Dirty affected);

    std::unique_ptr<Node> root_;
    ShapeText shaper_;
    contracts::ResourceId font_{};
    contracts::LogicalSize viewport_{};
    std::vector<Node *> nodes_;
    std::unordered_map<std::string, Node *> regions_;

    struct BindingTarget {
        Node *node;
        DslProperty property;
    };

    std::unordered_map<std::string, std::vector<BindingTarget>> bindings_;
    Dirty dirty_{Dirty::Layout | Dirty::Paint};
    std::uint64_t transaction_revision_{1};
    std::uint64_t generation_{0};
    std::uint64_t pixels_revision_{1};
    std::uint64_t build_calls_{0}, layout_count_{0};
    std::unique_ptr<RenderTree> render_tree_;
    Node *hovered_{nullptr};
    Node *focused_{nullptr};
    mutable bool input_dirty_{true};
    mutable std::vector<contracts::SurfaceInputRegion> input_regions_;
    std::optional<contracts::ThemeSnapshot> theme_;
};

} // namespace prism::runtime
