#include "client_application_p.hpp"

namespace prism::sdk {
contracts::ResourceId ClientApplication::Impl::RequestImage(std::set<std::uint64_t> &images,
                                                            std::string_view uri)
{
    auto &app = *this;
    ++app.requested_images;
    std::string path(uri);
    if (!app.config.assets_root.empty()) {
        const std::filesystem::path relative(path);
        if (relative.is_absolute() || path.find('\\') != std::string::npos) {
            throw std::invalid_argument("Invalid package image URI");
        }
        for (const auto &part : relative) {
            if (part == "..") {
                throw std::invalid_argument("Image traversal");
            }
        }
        const auto root = std::filesystem::canonical(app.config.assets_root);
        const auto resolved = std::filesystem::canonical(root / relative);
        auto a = root.begin(), b = resolved.begin();
        for (; a != root.end() && b != resolved.end() && *a == *b; ++a, ++b) {
        }
        if (a != root.end() || !std::filesystem::is_regular_file(resolved)) {
            throw std::invalid_argument("Image outside package assets");
        }
        path = resolved.string();
    } else if (!std::filesystem::exists(path)) {
        const auto development = std::filesystem::path("resources") / path;
        const auto installed =
            std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() /
            "share/prism" / path;
        if (std::filesystem::exists(development)) {
            path = development.string();
        } else if (std::filesystem::exists(installed)) {
            path = installed.string();
        }
    }

    auto id = app.resources.Request(std::move(path));
    images.insert(id.value);
    return id;
}

runtime::ShapedText ClientApplication::Impl::ShapeText(std::string_view text, double size)
{
    return commands.Shape(text, size);
}

bool ClientApplication::Impl::LoadScene(std::string_view dsl_source)
{
    auto &app = *this;

    std::set<std::uint64_t> images;
    try {
        auto next = std::make_unique<runtime::Scene>(
            runtime::ParseBlueprint(dsl_source,
                                    std::bind_front(&Impl::RequestImage, this, std::ref(images))),
            std::bind_front(&Impl::ShapeText, this), app.commands.FontId(), app.theme);

        if (app.window.IsConfigured()) {
            next->SetViewport(app.window.Metrics().logical_size);
        }

        for (auto value : images) {
            const contracts::ResourceId id{value};
            if (app.resources.State(id) == runtime::ImageState::Failed) {
                throw std::runtime_error("Package image unavailable");
            }
            if (const auto *image = app.resources.Get(id)) {
                if (!app.commands.RegisterImage(id, *image) ||
                    !next->ImageReady(id, {static_cast<double>(image->width),
                                           static_cast<double>(image->height)})) {
                    throw std::runtime_error("Cached image registration failed");
                }
            }
        }

        if (app.scene) {
            AddSceneStats(app.render_stats, app.scene->GetRenderStats());
        }
        app.scene = std::move(next);
        app.scene_images = std::move(images);
        app.last_list.reset();
        app.committed_list.reset();
        app.prepared_list.reset();
        app.prepared_damage.reset();
        app.damage_history.Invalidate();
        app.committed_pixels_revision = app.prepared_pixels_revision = 0;
        app.state_prepared = false;
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

} // namespace prism::sdk
