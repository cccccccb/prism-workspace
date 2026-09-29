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
    if (!id) {
        throw std::runtime_error("Image resource queue is full");
    }
    images.insert(id.value);
    return id;
}

runtime::ShapedText ClientApplication::Impl::ShapeText(std::string_view text, double size)
{
    return shaper.Shape(text, size);
}

bool ClientApplication::Impl::InstallScene(runtime::UiLoadId load,
                                           const runtime::PreparedComponent &prepared,
                                           runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *this;

    if (!app.ui_load.Current(load) || app.closed || app.failed) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Cancelled,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "UI load is cancelled, superseded or belongs to another frontend"};
        }
        return false;
    }
    if (app.install) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "A staged UI installation is already pending"};
        }
        return false;
    }
    if (app.installed_ui == load) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "UI load has already been installed"};
        }
        return false;
    }

    std::set<std::uint64_t> images;
    try {
        auto next = std::make_unique<runtime::Scene>(
            runtime::LinkComponent(prepared,
                                   std::bind_front(&Impl::RequestImage, this, std::ref(images))),
            std::bind_front(&Impl::ShapeText, this), app.shaper.FontId(), app.theme);

        next->SetViewport(app.ui_configure_count
                              ? app.ui_metrics.logical_size
                              : contracts::LogicalSize{static_cast<double>(app.config.width),
                                                       static_cast<double>(app.config.height)});

        for (auto value : images) {
            const contracts::ResourceId id{value};
            if (app.resources.State(id) == runtime::ImageState::Failed) {
                throw std::runtime_error("Package image unavailable");
            }
            if (const auto *image = app.resources.Get(id)) {
                if (!app.RegisterImage(id) ||
                    !next->ImageReady(id, {static_cast<double>(image->width),
                                           static_cast<double>(image->height)})) {
                    throw std::runtime_error("Cached image registration failed");
                }
            }
        }

        std::string failure;
        if (!next->PrepareDetached(app.binding_values, &failure)) {
            throw std::runtime_error(failure);
        }
        app.CommitScene(load, std::move(next), std::move(images));
    } catch (const runtime::LoadFailure &error) {
        for (auto value : images) {
            if (!app.scene_images.contains(value) && !app.preloaded_images.contains(value)) {
                app.DropImage({value});
            }
        }
        if (diagnostic) {
            *diagnostic = error.Diagnostic();
        }
        return false;
    } catch (const std::exception &error) {
        for (auto value : images) {
            if (!app.scene_images.contains(value) && !app.preloaded_images.contains(value)) {
                app.DropImage({value});
            }
        }
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           error.what()};
        }
        return false;
    }

    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}

} // namespace prism::sdk
