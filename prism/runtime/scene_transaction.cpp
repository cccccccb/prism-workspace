#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/render_tree.hpp"
#include "prism/runtime/scene_construction.hpp"
#include "scene_p.hpp"
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
void ReplaceRegions(Blueprint &node, std::span<const RegionUpdate> updates,
                    std::set<std::string> &found, std::size_t &count, std::size_t depth = 1,
                    bool inside_update = false)
{
    if (++count > 8192 || depth > 64) {
        throw std::length_error("Combined scene node or depth limit");
    }
    for (const auto &update : updates) {
        if (node.region != update.region) {
            continue;
        }
        if (inside_update || node.region_mounted || !found.insert(update.region).second) {
            throw std::invalid_argument("Region is already mounted or overlaps another update");
        }
        node.children = {update.content};
        node.region_mounted = true;
        inside_update = true;
        break;
    }
    for (auto &child : node.children) {
        ReplaceRegions(child, updates, found, count, depth + 1, inside_update);
    }
}

bool SameStructure(const Blueprint &a, const Blueprint &b)
{
    if (a.kind != b.kind || a.region != b.region || a.region_mounted != b.region_mounted ||
        a.allowed_properties != b.allowed_properties || a.transitions != b.transitions ||
        a.bindings.size() != b.bindings.size() || a.children.size() != b.children.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        if (a.bindings[i].name != b.bindings[i].name ||
            a.bindings[i].target != b.bindings[i].target) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!SameStructure(a.children[i], b.children[i])) {
            return false;
        }
    }
    return true;
}

void Success(std::string *diagnostic)
{
    if (diagnostic) {
        diagnostic->clear();
    }
}

bool Failure(std::string *diagnostic, const std::exception &error)
{
    if (diagnostic) {
        *diagnostic = error.what();
    }
    return false;
}
} // namespace

contracts::NodeId Scene::RegionId(std::string_view region) const
{
    const auto found = regions_.find(std::string(region));
    return found == regions_.end() ? contracts::NodeId{} : found->second->id;
}

bool Scene::RegionMounted(std::string_view region) const
{
    const auto found = regions_.find(std::string(region));
    return found != regions_.end() && found->second->region_mounted;
}

Blueprint Scene::RegionBlueprint(std::span<const RegionUpdate> updates) const
{
    if (!root_ || updates.empty()) {
        throw std::invalid_argument("Empty region transaction");
    }
    std::set<std::string> names;
    for (const auto &update : updates) {
        const auto found = regions_.find(update.region);
        if (update.region.empty() || !names.insert(update.region).second ||
            found == regions_.end() || found->second->region_mounted) {
            throw std::invalid_argument("Unknown, duplicate or already mounted region");
        }
    }
    auto blueprint = CurrentBlueprint(*root_);
    std::set<std::string> found;
    std::size_t count = 0;
    ReplaceRegions(blueprint, updates, found, count);
    if (found.size() != updates.size()) {
        throw std::invalid_argument("Overlapping region transaction");
    }
    return blueprint;
}

void Scene::CollectPairs(Node &live, Node &candidate,
                         std::vector<std::pair<Node *, Node *>> &pairs) const
{
    pairs.emplace_back(&live, &candidate);
    if (live.children.size() != candidate.children.size()) {
        throw std::invalid_argument("Candidate topology differs from scene");
    }
    for (std::size_t i = 0; i < live.children.size(); ++i) {
        CollectPairs(*live.children[i], *candidate.children[i], pairs);
    }
}

void Scene::CollectMountPairs(Node &live, Node &candidate, const std::set<std::string> &updates,
                              std::vector<std::pair<Node *, Node *>> &pairs) const
{
    pairs.emplace_back(&live, &candidate);
    if (updates.contains(live.region)) {
        return;
    }
    if (live.children.size() != candidate.children.size()) {
        throw std::invalid_argument("Candidate topology differs outside region");
    }
    for (std::size_t i = 0; i < live.children.size(); ++i) {
        CollectMountPairs(*live.children[i], *candidate.children[i], updates, pairs);
    }
}

void Scene::CollectNodes(Node &node, std::vector<Node *> &nodes) const
{
    nodes.push_back(&node);
    for (auto &child : node.children) {
        CollectNodes(*child, nodes);
    }
}

void Scene::CopyResources(const Node &live, Node &candidate) const
{
    if (live.image == candidate.image) {
        candidate.intrinsic_size = live.intrinsic_size;
        candidate.image_ready = live.image_ready;
    }
    if (live.children.size() != candidate.children.size()) {
        throw std::invalid_argument("Resource candidate topology differs");
    }
    for (std::size_t i = 0; i < live.children.size(); ++i) {
        CopyResources(*live.children[i], *candidate.children[i]);
    }
}

void Scene::ValidateCandidate(Scene &candidate, const BindingValues &values) const
{
    if (!candidate.root_) {
        throw std::invalid_argument("Scene candidate has been consumed");
    }
    for (const auto &[name, value] : values) {
        if (candidate.bindings_.contains(name) && !candidate.AcceptsBinding(name, value)) {
            throw std::invalid_argument("Invalid mounted binding value: " + name);
        }
    }
    for (const auto &[name, value] : values) {
        if (candidate.bindings_.contains(name)) {
            candidate.SetBinding(name, value);
        }
    }
    if (scene_detail::ValidSize(viewport_)) {
        candidate.SetViewport(viewport_);
        candidate.dirty_ = candidate.dirty_ | Dirty::Layout | Dirty::Paint;
        candidate.hit_geometry_dirty_ = true;
        candidate.input_dirty_ = true;
        const auto list = candidate.Build({1});
        if (!list) {
            throw std::invalid_argument("Candidate display list is unavailable");
        }
        contracts::ValidateDisplayList(*list);
        for (const auto *node : candidate.nodes_) {
            if (!node) {
                continue;
            }
            const auto &shape = node->shaped;
            const auto &bounds = node->bounds;
            if (!std::isfinite(shape.width) || !std::isfinite(shape.height) || shape.width < 0 ||
                shape.height < 0 || !std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
                !std::isfinite(bounds.width) || !std::isfinite(bounds.height) || bounds.width < 0 ||
                bounds.height < 0) {
                throw std::invalid_argument("Invalid candidate shaping or layout metrics");
            }
        }
        (void)candidate.SurfaceEffects();
        (void)candidate.InputRegions();
    } else {
        std::size_t effects = 0;
        for (const auto *node : candidate.nodes_) {
            if (node && candidate.IsVisible(*node) && node->style.backdrop_blur > 0) {
                ++effects;
            }
        }
        if (effects > 8) {
            throw std::length_error("Surface effect region limit is 8");
        }
    }
}

void Scene::ValidateRetainedValues(const std::vector<std::pair<Node *, Node *>> &pairs,
                                   const BindingValues &values) const
{
    for (const auto &[live, candidate] : pairs) {
        // Multiple binding names for one target retain the same lexicographic
        // projection order as ValidateCandidate's ordered BindingValues map.
        std::map<DslProperty, std::pair<std::string, const PropertyValue *>> projected;
        for (const auto &binding : live->bindings) {
            const auto value = values.find(binding.name);
            if (value == values.end()) {
                continue;
            }
            auto [position, inserted] =
                projected.emplace(binding.target, std::pair{binding.name, &value->second});
            if (!inserted && position->second.first < binding.name) {
                position->second = {binding.name, &value->second};
            }
        }
        for (unsigned id = 0; id <= static_cast<unsigned>(DslProperty::Visible); ++id) {
            const auto property = static_cast<DslProperty>(id);
            const auto projection = projected.find(property);
            const auto expected = projection == projected.end() ? CurrentProperty(*live, property)
                                                                : *projection->second.second;
            if (expected != CurrentProperty(*candidate, property)) {
                throw std::invalid_argument(
                    "Candidate changes retained node outside binding projection");
            }
        }
        if (live->explicit_properties != candidate->explicit_properties) {
            throw std::invalid_argument("Candidate changes retained property provenance");
        }
        if (live->transitions != candidate->transitions) {
            throw std::invalid_argument("Candidate changes retained transition descriptors");
        }
        auto expected_refs = live->theme_refs;
        for (const auto &[property, projection] : projected) {
            if (CurrentProperty(*live, property) != *projection.second) {
                std::erase_if(expected_refs,
                              [property](const ThemeRef &ref) { return ref.target == property; });
            }
        }
        if (expected_refs.size() != candidate->theme_refs.size()) {
            throw std::invalid_argument("Candidate changes retained theme references");
        }
        for (std::size_t i = 0; i < expected_refs.size(); ++i) {
            if (expected_refs[i].name != candidate->theme_refs[i].name ||
                expected_refs[i].target != candidate->theme_refs[i].target) {
                throw std::invalid_argument("Candidate changes retained theme references");
            }
        }
    }
}

void Scene::CommitValues(const std::vector<std::pair<Node *, Node *>> &pairs) noexcept
{
    for (const auto &[live, candidate] : pairs) {
        std::swap(live->style, candidate->style);
        live->properties.swap(candidate->properties);
        live->explicit_properties.swap(candidate->explicit_properties);
        live->theme_refs.swap(candidate->theme_refs);
        live->text.swap(candidate->text);
        live->action.swap(candidate->action);
        live->icon.swap(candidate->icon);
        live->value = candidate->value;
        live->checked = candidate->checked;
        live->image = candidate->image;
        live->intrinsic_size = candidate->intrinsic_size;
        live->image_ready = candidate->image_ready;
        live->bounds = candidate->bounds;
        live->shaped.glyphs.swap(candidate->shaped.glyphs);
        live->shaped.width = candidate->shaped.width;
        live->shaped.height = candidate->shaped.height;
        live->region_mounted = candidate->region_mounted;
        ++live->revision;
    }
}

bool Scene::PrepareDetached(const BindingValues &values, std::string *diagnostic)
{
    try {
        ValidateCandidate(*this, values);
        // Validation builds are not pixel submissions. Keep the validated
        // geometry, but leave a display-list build pending for installation.
        dirty_ = dirty_ | Dirty::Paint;
        Success(diagnostic);
        return true;
    } catch (const std::exception &error) {
        return Failure(diagnostic, error);
    }
}

bool Scene::Preflight(const BindingValues &values, std::string *diagnostic)
{
    try {
        if (!root_) {
            throw std::invalid_argument("Scene candidate has been consumed");
        }
        Scene candidate(CurrentBlueprint(*root_), shaper_, font_, theme_);
        CopyResources(*root_, *candidate.root_);
        ValidateCandidate(candidate, values);
        std::vector<std::pair<Node *, Node *>> pairs;
        CollectPairs(*root_, *candidate.root_, pairs);

        CommitValues(pairs);
        ReconcileCommittedAnimations(pairs);
        CancelHiddenAnimations();
        ++transaction_revision_;
        input_dirty_ = true;
        Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);
        ReconcileInput();
        Success(diagnostic);
        return true;
    } catch (const std::exception &error) {
        return Failure(diagnostic, error);
    }
}

bool Scene::MountRegions(std::span<const RegionUpdate> updates, const BindingValues &values,
                         std::string *diagnostic)
{
    try {
        const auto revision = transaction_revision_;
        Scene candidate(RegionBlueprint(updates), shaper_, font_, theme_);
        return MountRegions(updates, values, candidate, revision, diagnostic);
    } catch (const std::exception &error) {
        return Failure(diagnostic, error);
    }
}

bool Scene::MountRegions(std::span<const RegionUpdate> updates, const BindingValues &values,
                         Scene &candidate, std::uint64_t expected_revision, std::string *diagnostic)
{
    try {
        if (&candidate == this || expected_revision != transaction_revision_ ||
            candidate.theme_ != theme_ || candidate.font_ != font_) {
            throw std::invalid_argument("Stale or incompatible region candidate");
        }
        if (!candidate.root_) {
            throw std::invalid_argument("Scene candidate has been consumed");
        }
        const auto expected = RegionBlueprint(updates);
        if (!SameStructure(expected, candidate.CurrentBlueprint(*candidate.root_))) {
            throw std::invalid_argument("Region candidate does not match transaction topology");
        }
        std::set<std::string> names;
        for (const auto &update : updates) {
            names.insert(update.region);
        }
        std::vector<std::pair<Node *, Node *>> pairs;
        CollectMountPairs(*root_, *candidate.root_, names, pairs);
        // A detached candidate never supplies the live owner's shaping service.
        candidate.shaper_ = shaper_;
        for (const auto &[live, prepared] : pairs) {
            if (live->image == prepared->image) {
                prepared->intrinsic_size = live->intrinsic_size;
                prepared->image_ready = live->image_ready;
            }
        }
        ValidateCandidate(candidate, values);
        ValidateRetainedValues(pairs, values);

        auto next_nodes = nodes_;
        std::vector<std::pair<Node *, Node *>> replacements;
        std::vector<Node *> removed;
        for (const auto &name : names) {
            auto *live = regions_.at(name);
            auto *prepared = candidate.regions_.at(name);
            replacements.emplace_back(live, prepared);
            for (auto &child : live->children) {
                CollectNodes(*child, removed);
            }
            std::vector<Node *> added;
            for (auto &child : prepared->children) {
                CollectNodes(*child, added);
            }
            for (auto *node : added) {
                if (next_nodes.size() >= UINT32_MAX) {
                    throw std::length_error("Scene NodeId limit");
                }
                next_nodes.push_back(node);
            }
        }
        for (const auto *node : removed) {
            next_nodes[node->id.index] = nullptr;
        }
        std::unordered_map<std::string, std::vector<BindingTarget>> next_bindings;
        std::unordered_map<std::string, Node *> next_regions;
        for (auto *node : next_nodes) {
            if (!node) {
                continue;
            }
            for (const auto &binding : node->bindings) {
                next_bindings[binding.name].push_back({node, binding.target});
            }
            if (!node->region.empty() && !next_regions.emplace(node->region, node).second) {
                throw std::invalid_argument("Duplicate mounted region");
            }
        }
        // Every allocation, projection and whole-scene validation has completed.
        // These moves preserve retained node addresses and cannot fail.
        CommitValues(pairs);
        ReconcileCommittedAnimations(pairs);
        for (auto &[live, prepared] : replacements) {
            live->children.swap(prepared->children);
            for (auto &child : live->children) {
                child->parent = live;
            }
        }
        for (std::size_t i = nodes_.size(); i < next_nodes.size(); ++i) {
            next_nodes[i]->id = {static_cast<std::uint32_t>(i), 1};
        }
        nodes_.swap(next_nodes);
        DropAnimationsForNodes(removed);
        CancelHiddenAnimations();
        bindings_.swap(next_bindings);
        regions_.swap(next_regions);
        ReconcileInput();
        candidate.nodes_.clear();
        candidate.bindings_.clear();
        candidate.regions_.clear();
        candidate.root_.reset();
        candidate.render_tree_.reset();
        render_tree_.reset();
        input_dirty_ = true;
        ++transaction_revision_;
        Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);
        Success(diagnostic);
        return true;
    } catch (const std::exception &error) {
        return Failure(diagnostic, error);
    }
}
} // namespace prism::runtime
