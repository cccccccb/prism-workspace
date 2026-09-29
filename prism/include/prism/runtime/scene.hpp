#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/events.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/runtime/animation_sample.hpp"
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

namespace prism::animation {
class AnimationClock;
}

namespace prism::runtime {
struct RenderTree;
struct SnapshotNode;

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

// Identities are local to this Scene; the SDK owns the enclosing UI load identity.
struct HitResult {
    contracts::NodeId node{};
    contracts::LogicalPoint localPosition{};
};

struct InteractionState {
    bool hovered{}, pressed{}, captured{}, focused{}, focusVisible{};
    bool enabled{true};
    bool operator==(const InteractionState &) const = default;
};

struct Activation {
    contracts::NodeId node{};
    std::string action;
};

struct InteractionResult {
    bool changed{};
    std::optional<Activation> activation;
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
    // Enable transitions only after the initial Prepared UI has been installed.
    // A supplied clock must outlive this Scene.
    void EnableAnimations(const animation::AnimationClock *clock = nullptr);
    bool HasActiveAnimations() const noexcept;
    std::uint64_t AnimationNowNs() const noexcept;
    bool AdvanceAnimations(std::uint64_t now);
    // Resolves local input state without changing authored bindings or theme references.
    void ResolveInteractionStyles();
    std::optional<std::uint64_t> NextAnimationDeadlineNs(std::uint64_t now) const noexcept;

    AnimationSampleStamp AnimationSample() const noexcept
    {
        return animation_sample_;
    }

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
    std::optional<HitResult> HitTest(contracts::LogicalPoint point) const;
    InteractionResult HandleInput(const contracts::WindowEvent &event);
    InteractionState State(contracts::NodeId id) const;
    // Typed runtime control; this does not introduce an enabled DSL property.
    bool SetEnabled(contracts::NodeId id, bool enabled);
    bool CancelInput();

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
    // Legacy convenience entry points use the default source / seat zero.
    // Platform events and action dispatch go through HandleInput.
    bool SetPointer(contracts::LogicalPoint point);
    bool FocusNext();
    std::optional<std::string> FocusedAction() const;

private:
    friend class SceneConstruction;
    struct Node;
    struct AnimationState;
    struct InputState;

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
    bool SetPropertyAt(contracts::NodeId id, DslProperty property, PropertyValue value,
                       std::uint64_t now);
    bool RetargetPresentation(Node &node, DslProperty property, const PropertyValue &previous,
                              const PropertyValue &target, std::uint64_t now);
    void ApplyPresentation(const Node &node, SnapshotNode &snapshot) const;
    void PrepareNodeStates(Node &, std::vector<StateRule>);
    void PrepareInteractionTree();
    void ValidateInteractionTree(const Node &, const Node *, bool) const;
    void BindInteractionTree(Node &, Node *, bool) noexcept;
    bool IsStateProperty(const Node &, DslProperty) const noexcept;
    PropertyValue EffectiveProperty(const Node &, DslProperty) const;
    void ResolveStateTargets(std::uint64_t now, bool animate) noexcept;
    void ResolveNodeStateTargets(Node &, std::uint64_t now, bool animate) noexcept;
    void ApplySnapshotProperty(SnapshotNode &, DslProperty, const PropertyValue &) const;
    void RecordAnimationSample(std::uint64_t now) noexcept;
    void CancelAnimations() noexcept;
    void CancelHiddenAnimations() noexcept;
    void ReconcileCommittedAnimations(const std::vector<std::pair<Node *, Node *>> &pairs) noexcept;
    void DropAnimationsForNodes(const std::vector<Node *> &nodes) noexcept;
    void ApplyCachedProperty(Node &node, DslProperty property, const PropertyValue &value);
    PropertyValue CurrentProperty(const Node &node, DslProperty property) const;
    std::optional<HitResult> Hit(const Node &node, contracts::LogicalPoint point) const;
    bool IsEnabled(const Node &node) const;
    bool IsInteractive(contracts::NodeId id) const;
    void TrackInputTarget(contracts::NodeId id);
    bool RefreshInputStates() noexcept;
    bool ReconcileInput() noexcept;
    void PrepareInputGeometry();
    void RefreshInputGeometry() noexcept;
    void CancelSeatInput(std::uint64_t seat) noexcept;
    bool SetInputFocus(contracts::NodeId id, std::uint64_t seat, bool visible);
    bool MoveInputFocus(std::uint64_t seat, bool reverse);
    void MoveInputPointer(contracts::InputSource source, contracts::LogicalPoint point);
    void LeaveInputPointer(contracts::InputSource source, bool cancel);
    std::optional<Activation> HandleInputButton(const contracts::PointerButtonEvent &);
    std::optional<Activation> HandleInputKey(const contracts::KeyEvent &);
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
    std::unique_ptr<AnimationState> animation_state_;
    bool state_styles_dirty_{true};
    std::unique_ptr<InputState> input_state_;
    AnimationSampleStamp animation_sample_{};
    bool hit_geometry_dirty_{true};
    mutable bool input_dirty_{true};
    mutable std::vector<contracts::SurfaceInputRegion> input_regions_;
    std::optional<contracts::ThemeSnapshot> theme_;
};

} // namespace prism::runtime
