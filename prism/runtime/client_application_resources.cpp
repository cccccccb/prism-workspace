#include "client_application_p.hpp"

namespace prism::sdk {
void ClientApplication::Impl::ReleaseUnusedImages(const std::set<std::uint64_t> &keep)
{
    std::set<std::uint64_t> previous = scene_images;
    previous.insert(preloaded_images.begin(), preloaded_images.end());
    for (auto value : previous) {
        if (!keep.contains(value)) {
            const contracts::ResourceId id{value};
            DropImage(id);
        }
    }
}

void ClientApplication::Impl::ClearPreloadedImages()
{
    for (auto value : preloaded_images) {
        if (!scene_images.contains(value) && (!install || !install->owned_images.contains(value))) {
            DropImage({value});
        }
    }
    preloaded_images.clear();
    preloaded_ui = {};
}

bool ClientApplication::PreloadImages(runtime::UiLoadId load,
                                      const runtime::PreparedComponent &prepared,
                                      runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (!prepared || !app.ui_load.Current(load) || app.failed || app.closed || app.install) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Cancelled,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "Cannot preload resources for an obsolete UI load"};
        }
        return false;
    }
    app.ClearPreloadedImages();
    app.preloaded_ui = load;
    try {
        for (const auto &image : prepared.Images()) {
            app.RequestImage(app.preloaded_images, image.uri);
        }
    } catch (const std::exception &error) {
        app.ClearPreloadedImages();
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::ResourceLink, prepared.Source(), 0, error.what()};
        }
        return false;
    }
    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}

runtime::ImageState ClientApplication::PreloadedImageState(runtime::UiLoadId load) const
{
    const auto &app = *impl_;
    if (app.preloaded_ui != load || !app.ui_load.Current(load)) {
        return runtime::ImageState::Failed;
    }
    bool loading = false;
    for (auto value : app.preloaded_images) {
        const auto state = app.resources.State({value});
        if (state == runtime::ImageState::Failed) {
            return state;
        }
        loading |= state == runtime::ImageState::Loading;
    }
    return loading ? runtime::ImageState::Loading : runtime::ImageState::Ready;
}

int ClientApplication::ResourceCompletionFd() const noexcept
{
    return impl_->resources.CompletionFd();
}

bool ClientApplication::PollImageResources()
{
    return impl_->PollResources();
}
} // namespace prism::sdk
