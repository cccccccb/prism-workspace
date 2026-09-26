#pragma once
#include "prism/runtime/property.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace prism::sdk {

std::optional<std::string> LoadUiSource(std::string_view installed_name,
                                        std::string_view source_path);

struct ClientConfig {
    std::string socket;
    std::string app_id;
    std::string title;
    std::string font_path;
    int width{640};
    int height{400};
};

// Client-owned DSL scene, resources, Skia GLES renderer and Wayland window.
// All methods except construction/destruction run on the Wayland thread.
class ClientApplication {
public:
    explicit ClientApplication(ClientConfig config);
    ~ClientApplication();
    ClientApplication(const ClientApplication&) = delete;
    ClientApplication& operator=(const ClientApplication&) = delete;

    bool Open(std::string_view dsl_source);
    bool Pump(int timeout_ms);
    bool SetSlot(std::string_view name, std::string value);
    bool SetBinding(std::string_view name, runtime::PropertyValue value);
    void OnAction(std::function<void(std::string_view)> callback);
    bool IsCloseRequested() const;
    bool IsMapped() const;
    int ConfigureCount() const;
    int FrameDoneCount() const;
    int PresentedCount() const;
    int RequestedImageCount() const;
    int LoadedImageCount() const;
    std::string GlRenderer() const;
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::sdk
