#include "prism/wm/window.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include <cmath>
#include <utility>

namespace prism::wm {
Window::Window(std::string app_id, std::string title, core::Rect bounds,
               std::shared_ptr<ipc::Channel> channel)
    : app_id_(std::move(app_id)), title_(std::move(title)), bounds_(bounds),
      channel_(std::move(channel))
{
}

Window::~Window() = default;

bool Window::SetMinimumSize(float width, float height)
{
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0) {
        return false;
    }

    minimum_width_ = width;
    minimum_height_ = height;
    return true;
}

void Window::SetBounds(core::Rect bounds)
{
    bounds_ = bounds;
    if (decorator_) {
        decorator_->ApplyGeometry(bounds);
    }
}

void Window::SetFocused(bool focused)
{
    is_focused_ = focused;
    if (decorator_) {
        decorator_->SetFocused(focused);
    }
}

void Window::SetDecorator(std::unique_ptr<decoration::TilingWindowDecorator> decorator)
{
    decorator_ = std::move(decorator);
    if (decorator_) {
        decorator_->SetFocused(is_focused_);
        decorator_->ApplyGeometry(bounds_);
    }
}

} // namespace prism::wm
