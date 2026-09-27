#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/prepared_component.hpp"

#include <cassert>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace prism;

runtime::ComponentSource Source()
{
    return {"transport", "ui/transport.prism", "revision-7"};
}

void CheckSource(const runtime::ComponentSource &actual, const runtime::ComponentSource &expected)
{
    assert(actual.component_id == expected.component_id);
    assert(actual.source_path == expected.source_path);
    assert(actual.source_version == expected.source_version);
}

void CheckDiagnostic(const runtime::LoadFailure &failure, runtime::LoadStage stage,
                     const runtime::ComponentSource &source, int line)
{
    const auto &diagnostic = failure.Diagnostic();
    assert(diagnostic.stage == stage);
    CheckSource(diagnostic.source, source);
    assert(diagnostic.line == line);
    assert(!diagnostic.message.empty());
    assert(!std::string_view(failure.what()).empty());
}

const runtime::PropertyValue &Property(const runtime::Blueprint &node,
                                       runtime::DslProperty property)
{
    for (auto assignment = node.properties.rbegin(); assignment != node.properties.rend();
         ++assignment) {
        if (assignment->id == property) {
            return assignment->value;
        }
    }
    throw std::logic_error("Expected property was absent");
}

bool HasBinding(const runtime::Blueprint &node, std::string_view name,
                runtime::DslProperty property)
{
    for (const auto &binding : node.bindings) {
        if (binding.name == name && binding.target == property) {
            return true;
        }
    }
    return false;
}

bool HasThemeRef(const runtime::Blueprint &node, std::string_view name,
                 runtime::DslProperty property)
{
    for (const auto &reference : node.theme_refs) {
        if (reference.name == name && reference.target == property) {
            return true;
        }
    }
    return false;
}

struct PrepareJob {
    std::string source;
    runtime::ComponentSource origin;
    std::thread::id worker;
    std::promise<runtime::PreparedComponent> result;

    void Run()
    {
        try {
            worker = std::this_thread::get_id();
            auto prepared = runtime::PrepareComponent(source, origin);

            // No text or metadata may refer to the caller's mutable buffers.
            source.assign(source.size() * 8, 'x');
            source.clear();
            source.shrink_to_fit();
            origin.component_id.clear();
            origin.source_path.clear();
            origin.source_version.clear();

            result.set_value(std::move(prepared));
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    }
};

runtime::PreparedComponent PrepareOnWorker(std::string source, runtime::ComponentSource origin)
{
    PrepareJob job{std::move(source), std::move(origin), {}, {}};
    auto future = job.result.get_future();
    const auto owner = std::this_thread::get_id();

    std::jthread worker(&PrepareJob::Run, &job);
    auto prepared = future.get();
    worker.join();

    assert(job.worker != owner);
    return prepared;
}

enum class ResolverMode { Valid, Empty, Throw, NonStandardThrow };

struct ResolverState {
    std::thread::id owner{std::this_thread::get_id()};
    contracts::ResourceId id;
    ResolverMode mode{ResolverMode::Valid};
    std::vector<std::string> requested;

    contracts::ResourceId Resolve(std::string_view uri)
    {
        assert(std::this_thread::get_id() == owner);
        requested.emplace_back(uri);
        if (mode == ResolverMode::Empty) {
            return {};
        }
        if (mode == ResolverMode::Throw) {
            throw std::runtime_error("Resource endpoint failed");
        }
        if (mode == ResolverMode::NonStandardThrow) {
            throw 42;
        }
        return id;
    }
};

struct ImageResolver {
    ResolverState *state;

    contracts::ResourceId operator()(std::string_view uri) const
    {
        return state->Resolve(uri);
    }
};

void CheckLinkedSemantics(const runtime::Blueprint &root, contracts::ResourceId image)
{
    assert(root.kind == runtime::Kind::Column);
    assert(root.children.size() == 5);
    assert(std::get<double>(Property(root, runtime::DslProperty::Spacing)) == 4);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto &node = root.children[index];
        assert(node.kind == runtime::Kind::Image);
        assert(std::get<contracts::ResourceId>(Property(node, runtime::DslProperty::Source)) ==
               image);
    }

    const auto &button = root.children[2];
    assert(button.kind == runtime::Kind::Box && button.children.size() == 1);
    assert(std::get<std::string>(Property(button, runtime::DslProperty::Action)) == "music:play");
    assert(button.children[0].kind == runtime::Kind::Text);
    assert(HasBinding(button.children[0], "caption", runtime::DslProperty::Text));
    assert(!HasBinding(button, "caption", runtime::DslProperty::Text));
    assert(HasThemeRef(button.children[0], "titleSize", runtime::DslProperty::Font));
    assert(HasThemeRef(button.children[0], "accent", runtime::DslProperty::Foreground));
    assert(HasBinding(root.children[3], "artist", runtime::DslProperty::Text));
    assert(HasBinding(root.children[4], "progress", runtime::DslProperty::Value));
}

void WorkerPreparationAndIndependentLinks()
{
    const std::string source = R"(
VStack(spacing:4) {
    Image("assets/cover.png",width:20,height:20)
    Image("assets/cover.png",width:20,height:20)
    Button($caption,"music:play",font:"@titleSize",foreground:"@accent")
    Text($artist,font:11)
    Progress(value:$progress,width:80,height:4)
})";
    const auto prepared = PrepareOnWorker(source, Source());
    assert(static_cast<bool>(prepared));
    CheckSource(prepared.Source(), Source());
    assert(prepared.SourceBytes() == source.size());
    assert(prepared.NodeCount() == 7);
    assert(prepared.Images().size() == 1);
    assert(prepared.Images()[0].uri == "assets/cover.png");
    assert(prepared.Images()[0].line == 3);
    for (const auto &node : prepared.Root().children) {
        if (node.kind != runtime::Kind::Image) {
            continue;
        }
        bool symbolic_source = false;
        for (const auto &property : node.properties) {
            if (property.id == runtime::DslProperty::Source) {
                assert(std::get<runtime::ImageReference>(property.value).resource_key == 0);
                symbolic_source = true;
            }
        }
        assert(symbolic_source);
    }

    const auto shared = prepared;
    ResolverState first_resolver{{std::this_thread::get_id()}, {31}, ResolverMode::Valid, {}};
    auto first = runtime::LinkComponent(shared, ImageResolver{&first_resolver});
    assert(first_resolver.requested == std::vector<std::string>{"assets/cover.png"});
    CheckLinkedSemantics(first, {31});

    // Linking creates independent instances; even editing one result must not
    // change the shared template or a later instance's values and bindings.
    for (auto &property : first.children[0].properties) {
        if (property.id == runtime::DslProperty::Width) {
            property.value = 999.0;
            break;
        }
    }
    assert(std::get<double>(Property(first.children[0], runtime::DslProperty::Width)) == 999);
    first.children[2].children[0].bindings.clear();
    ResolverState second_resolver{{std::this_thread::get_id()}, {72}, ResolverMode::Valid, {}};
    auto second = runtime::LinkComponent(prepared, ImageResolver{&second_resolver});
    assert(second_resolver.requested == std::vector<std::string>{"assets/cover.png"});
    CheckLinkedSemantics(second, {72});
    assert(std::get<double>(Property(second.children[0], runtime::DslProperty::Width)) == 20);
    assert(std::get<contracts::ResourceId>(Property(
               first.children[0], runtime::DslProperty::Source)) == contracts::ResourceId{31});
}

struct RejectedSource {
    const char *source;
    runtime::LoadStage stage;
    int line;
};

void PreparationDiagnostics()
{
    const RejectedSource cases[] = {
        {"VStack {\n Text(\"x\")\n Text(\"y\",width:)\n}", runtime::LoadStage::Syntax, 3},
        {"VStack {\n Text(\"x\",width:@)\n}", runtime::LoadStage::Syntax, 2},
        {"VStack {\n Text(\"x\",mystery:1)\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Text(\"x\",font:0)\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Text(\"x\",font:12,font:13)\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Image(\"cover\",fit:\"invalid\")\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Image(\"\")\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Card(align:\"invalid\")\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Icon(\"missing-icon\")\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Image(\"cover\") { Text(\"bad\") }\n}", runtime::LoadStage::Semantic, 2},
        {"VStack {\n Card(material:$dynamic)\n}", runtime::LoadStage::Semantic, 2},
    };
    for (const auto &test : cases) {
        try {
            (void)runtime::PrepareComponent(test.source, Source());
            assert(false);
        } catch (const runtime::LoadFailure &failure) {
            CheckDiagnostic(failure, test.stage, Source(), test.line);
        }
    }
}

void CompleteSemanticsBeforeResourceRequests()
{
    ResolverState resolver{{std::this_thread::get_id()}, {21}, ResolverMode::Valid, {}};
    const char *source = "VStack {\n Image(\"cover\")\n Text(\"bad\",mystery:1)\n}";
    try {
        (void)runtime::ParseBlueprint(source, ImageResolver{&resolver});
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        CheckDiagnostic(failure, runtime::LoadStage::Semantic, {}, 3);
    }
    assert(resolver.requested.empty());
}

void ResourceLinkDiagnostics()
{
    const auto prepared = runtime::PrepareComponent("VStack {\n Image(\"cover\")\n}", Source());
    try {
        (void)runtime::LinkComponent(prepared);
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        CheckDiagnostic(failure, runtime::LoadStage::ResourceLink, Source(), 2);
    }

    for (const auto mode :
         {ResolverMode::Empty, ResolverMode::Throw, ResolverMode::NonStandardThrow}) {
        ResolverState resolver{{std::this_thread::get_id()}, {0}, mode, {}};
        try {
            (void)runtime::LinkComponent(prepared, ImageResolver{&resolver});
            assert(false);
        } catch (const runtime::LoadFailure &failure) {
            CheckDiagnostic(failure, runtime::LoadStage::ResourceLink, Source(), 2);
        }
        assert(resolver.requested == std::vector<std::string>{"cover"});
    }

    ResolverState recovered{{std::this_thread::get_id()}, {42}, ResolverMode::Valid, {}};
    const auto linked = runtime::LinkComponent(prepared, ImageResolver{&recovered});
    assert(std::get<contracts::ResourceId>(Property(
               linked.children[0], runtime::DslProperty::Source)) == contracts::ResourceId{42});
}

void ImageFreeSingleFileCompatibility()
{
    const char *source = "HStack(spacing:6) { Button(\"Launch\",\"app:launch\") Text($title) }";
    const auto prepared = runtime::PrepareComponent(source);
    ResolverState unused{{std::this_thread::get_id()}, {1}, ResolverMode::Valid, {}};
    const auto linked = runtime::LinkComponent(prepared, ImageResolver{&unused});
    assert(unused.requested.empty());
    assert(prepared.Images().empty());
    assert(linked.kind == runtime::Kind::Row && linked.children.size() == 2);
    assert(std::get<double>(Property(linked, runtime::DslProperty::Spacing)) == 6);
    assert(std::get<std::string>(
               Property(linked.children[0].children[0], runtime::DslProperty::Text)) == "Launch");
    assert(HasBinding(linked.children[1], "title", runtime::DslProperty::Text));

    const auto bound_image = runtime::PrepareComponent("Image(source:$picture)");
    const auto bound_blueprint = runtime::LinkComponent(bound_image, ImageResolver{&unused});
    assert(bound_image.Images().empty());
    assert(unused.requested.empty());
    assert(HasBinding(bound_blueprint, "picture", runtime::DslProperty::Source));

    const auto old_entry = runtime::ParseBlueprint(source);
    assert(old_entry.kind == runtime::Kind::Row && old_entry.children.size() == 2);
    assert(std::get<double>(Property(old_entry, runtime::DslProperty::Spacing)) == 6);
    assert(std::get<std::string>(Property(old_entry.children[0], runtime::DslProperty::Action)) ==
           "app:launch");
    assert(std::get<std::string>(Property(old_entry.children[0].children[0],
                                          runtime::DslProperty::Text)) == "Launch");
    assert(HasBinding(old_entry.children[1], "title", runtime::DslProperty::Text));
    assert(prepared.NodeCount() == 4);
}

std::string EffectSource(unsigned count, bool image)
{
    std::string source = "VStack {\n";
    if (image) {
        source += " Image(\"cover\")\n";
    }
    for (unsigned index = 0; index < count; ++index) {
        source += " Card(backdropBlur:8,width:10,height:10)\n";
    }
    return source + "}";
}

void EffectLimit()
{
    const auto allowed = runtime::PrepareComponent(EffectSource(8, false), Source());
    assert(runtime::LinkComponent(allowed).children.size() == 8);
    try {
        (void)runtime::PrepareComponent(EffectSource(9, false), Source());
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        const auto &diagnostic = failure.Diagnostic();
        assert(diagnostic.stage == runtime::LoadStage::Semantic);
        CheckSource(diagnostic.source, Source());
        assert(diagnostic.line > 0 && !diagnostic.message.empty());
    }

    ResolverState resolver{{std::this_thread::get_id()}, {1}, ResolverMode::Valid, {}};
    try {
        (void)runtime::ParseBlueprint(EffectSource(9, true), ImageResolver{&resolver});
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        assert(failure.Diagnostic().stage == runtime::LoadStage::Semantic);
    }
    assert(resolver.requested.empty());
}

void MaterialReferencesAreNotResolvedDuringPreparation()
{
    std::string source = "VStack {\n";
    for (unsigned index = 0; index < 9; ++index) {
        source += " Card(material:\"control\")\n";
    }
    source += "}";

    // A material name does not tell the pure compiler whether the installed
    // theme gives it backdrop blur. The owner validates resolved effects.
    const auto prepared = runtime::PrepareComponent(source, Source());
    assert(prepared.NodeCount() == 10 && prepared.Images().empty());
    const auto linked = runtime::LinkComponent(prepared);
    assert(linked.children.size() == 9);
    for (const auto &node : linked.children) {
        assert(std::get<std::string>(Property(node, runtime::DslProperty::Material)) == "control");
    }
}

void SourceByteLimit()
{
    const std::string oversized(1024 * 1024 + 1, ' ');
    try {
        (void)runtime::PrepareComponent(oversized, Source());
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        CheckDiagnostic(failure, runtime::LoadStage::Semantic, Source(), 0);
    }
}

std::string ButtonSource(unsigned count)
{
    std::string source = "VStack {\n";
    for (unsigned index = 0; index < count; ++index) {
        source += " Button(\"Go\",\"go\")\n";
    }
    return source + "}";
}

void ExpandedNodeLimit()
{
    // Each Button introduces an additional label. Both input trees fit the
    // syntax-node budget, but 4096 buttons expand to 8193 runtime nodes.
    const auto accepted = runtime::PrepareComponent(ButtonSource(4095), Source());
    assert(accepted.NodeCount() == 8191);
    assert(accepted.Root().children.size() == 4095);
    assert(accepted.Images().empty());
    for (const auto &button : accepted.Root().children) {
        assert(button.children.size() == 1);
        assert(button.children[0].kind == runtime::Kind::Text);
    }

    try {
        (void)runtime::PrepareComponent(ButtonSource(4096), Source());
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        const auto &diagnostic = failure.Diagnostic();
        assert(diagnostic.stage == runtime::LoadStage::Semantic);
        CheckSource(diagnostic.source, Source());
        assert(diagnostic.line > 0 && !diagnostic.message.empty());
    }
}

void MovedFromComponentCannotLink()
{
    auto original = runtime::PrepareComponent("Image(\"cover\")", Source());
    auto moved = std::move(original);
    assert(!static_cast<bool>(original));
    assert(static_cast<bool>(moved));
    CheckSource(moved.Source(), Source());

    ResolverState resolver{{std::this_thread::get_id()}, {52}, ResolverMode::Valid, {}};
    try {
        (void)runtime::LinkComponent(original, ImageResolver{&resolver});
        assert(false);
    } catch (const runtime::LoadFailure &failure) {
        CheckDiagnostic(failure, runtime::LoadStage::ResourceLink, {}, 0);
    }
    assert(resolver.requested.empty());

    const auto linked = runtime::LinkComponent(moved, ImageResolver{&resolver});
    assert(resolver.requested == std::vector<std::string>{"cover"});
    assert(std::get<contracts::ResourceId>(Property(linked, runtime::DslProperty::Source)) ==
           contracts::ResourceId{52});
}
} // namespace

int main()
{
    WorkerPreparationAndIndependentLinks();
    PreparationDiagnostics();
    CompleteSemanticsBeforeResourceRequests();
    ResourceLinkDiagnostics();
    ImageFreeSingleFileCompatibility();
    EffectLimit();
    MaterialReferencesAreNotResolvedDuringPreparation();
    SourceByteLimit();
    ExpandedNodeLimit();
    MovedFromComponentCannotLink();
    std::cout << "prepared component ownership, linking, diagnostics and compatibility passed\n";
}
