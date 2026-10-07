#include "prism/runtime/popup.hpp"

#include <cassert>
#include <cmath>
#include <limits>

using namespace prism;
using namespace prism::runtime;

namespace {
void Placement()
{
    PopupPlacementRequest request{{0, 0, 800, 600}, {100, 100, 40, 32}, {280, 240}};
    auto result = PlacePopup(request);
    assert(result && result->side == PopupSide::Below);
    assert((result->bounds == contracts::LogicalRect{100, 140, 280, 240}));
    assert(result->effective_anchor == request.anchor);

    request.horizontal_alignment = PopupHorizontalAlignment::Center;
    result = PlacePopup(request);
    assert(result && result->bounds.x == 8);
    request.anchor.x = 300;
    result = PlacePopup(request);
    assert(result && result->bounds.x == 180 && result->effective_anchor == request.anchor);
    request.horizontal_alignment = static_cast<PopupHorizontalAlignment>(42);
    assert(!PlacePopup(request));
    request.horizontal_alignment = PopupHorizontalAlignment::Start;

    request.anchor = {-20, 580, 60, 80};
    result = PlacePopup(request);
    assert(result && result->side == PopupSide::Above);
    assert((result->effective_anchor == contracts::LogicalRect{0, 580, 40, 20}));
    assert(result->bounds.x == 8 && result->bounds.y == 332);

    request.anchor = {750, 500, 32, 32};
    result = PlacePopup(request);
    assert(result && result->side == PopupSide::Above && result->bounds.x == 512);
    assert(result->bounds.y == 252);

    request.desired.height = 500;
    result = PlacePopup(request);
    assert(result && result->height_constrained && result->bounds.height == 484);
    request.available = {0, 0, 140, 120};
    request.anchor = {40, 40, 20, 20};
    result = PlacePopup(request);
    assert(result && result->side == PopupSide::EdgePanel);
    assert((result->bounds == contracts::LogicalRect{8, 8, 124, 104}));

    request.anchor.x = 200;
    assert(!PlacePopup(request));
    request.anchor.x = 40;
    request.margin = 80;
    assert(!PlacePopup(request));
    request.margin = 8;
    request.gap = std::numeric_limits<double>::infinity();
    assert(!PlacePopup(request));
    request.gap = 8;
    request.anchor.width = std::numeric_limits<double>::quiet_NaN();
    assert(!PlacePopup(request));

    // Negative output origins and fractional scaling do not change the logical policy.
    for (double width : {180.5, 320.0, 640.25}) {
        for (double height : {120.0, 280.5, 600.0}) {
            for (double x : {0.0, .5, .95}) {
                for (double y : {0.0, .5, .95}) {
                    request = {{-700, -300, width, height},
                               {-692 + (width - 32) * x, -292 + (height - 32) * y, 16, 16},
                               {280, 240}};
                    result = PlacePopup(request);
                    assert(result);
                    const auto b = result->bounds;
                    assert(b.x >= -692 && b.y >= -292);
                    assert(b.x + b.width <= -700 + width - 8 + 1e-9);
                    assert(b.y + b.height <= -300 + height - 8 + 1e-9);
                    assert(b.width > 0 && b.height > 0);
                }
            }
        }
    }
}

void Lifecycle()
{
    PopupSession session(7);
    const PopupIdentity root{7, 11, {1, 1}};
    const PopupIdentity child{7, 11, {2, 1}};
    const PopupIdentity sibling{7, 11, {3, 1}};
    const contracts::NodeId focus{9, 1};
    assert(!session.Open({8, 11, {1, 1}}, focus));
    assert(!session.Open({7, 0, {1, 1}}, focus));
    assert(!session.Open({7, 11, {}}, focus));
    auto a = session.Open(root, focus);
    assert(a);
    assert(!session.Open(child, focus, *a + 1));
    assert(!session.Open({7, 12, {2, 1}}, focus, *a));
    assert(!session.Open(root, focus, *a));
    auto b = session.Open(child, {2, 1}, *a);
    assert(b && session.Entries().size() == 2);
    auto c = session.Open(sibling, {3, 1}, *a);
    assert(c && *c != *b && session.Entries().size() == 2);
    auto closures = session.TakeClosures();
    assert(closures.size() == 1 && closures[0].entry.token == *b);
    assert(closures[0].reason == PopupCloseReason::Replaced);
    assert(!session.Command(*b)); // A late command cannot close a replacement menu.
    assert(!session.Close(*b, PopupCloseReason::OutsidePress));
    assert(session.Escape() && session.Entries().size() == 1);
    closures = session.TakeClosures();
    assert(closures.size() == 1 && closures[0].entry.return_focus == contracts::NodeId(3, 1));
    b = session.Open(child, focus, *a);
    assert(b && session.Command(*b) && session.Entries().empty());
    closures = session.TakeClosures();
    assert(closures.size() == 2 && closures[0].entry.token == *b && closures[1].entry.token == *a);
    assert(closures[1].entry.return_focus == focus);
    assert(!session.Escape());

    a = session.Open(root, focus);
    b = session.Open(child, focus, *a);
    assert(!session.Invalidate({7, 11, {1, 2}})); // Reused index, different generation.
    assert(session.Invalidate(root) && session.Entries().empty());
    closures = session.TakeClosures();
    assert(closures.size() == 2 && closures[0].reason == PopupCloseReason::Unavailable);
    a = session.Open(root, focus);
    c = session.Open(sibling, focus);
    assert(a && c && session.Entries().size() == 1);
    closures = session.TakeClosures();
    assert(closures.size() == 1 && closures[0].entry.token == *a);
    session.CloseAll(PopupCloseReason::Unavailable);
    assert(session.Entries().empty());
}
} // namespace

int main()
{
    Placement();
    Lifecycle();
}
