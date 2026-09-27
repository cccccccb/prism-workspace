#pragma once
#include "prism/platform/wayland_window.hpp"
#include <sys/mman.h>

namespace prism::platform {
struct WaylandWindow::ShmBuffer {
    wl_buffer *handle{nullptr};
    void *pixels{MAP_FAILED};
    std::size_t bytes{0};
    int width{0};
    int height{0};
    bool busy{false};

    ~ShmBuffer()
    {
        if (handle) {
            wl_buffer_destroy(handle);
        }
        if (pixels != MAP_FAILED) {
            munmap(pixels, bytes);
        }
    }
};

} // namespace prism::platform
