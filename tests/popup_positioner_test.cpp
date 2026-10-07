#include "prism/contracts/popup_positioner.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace {
using namespace prism::contracts;

void CoordinateSpaces()
{
    const LogicalRect parent{-20, 30, 600, 360};
    PopupPositionerRequest request{{80.25, 90.75, 30.5, 16.5}, {280.2, 244.7}, 8.1};
    const auto plan = PreparePopupPositioner(request, parent);
    assert(plan);
    assert((plan->anchor == PopupPositionerRect{100, 60, 31, 18}));
    assert(plan->width == 281 && plan->height == 245 && plan->gap == 9);
    assert(plan->horizontal_alignment == PopupHorizontalAlignment::Center);
    assert(plan->vertical_preference == PopupVerticalPreference::Below);

    // Translating both inputs changes neither wire geometry nor placement preferences.
    const LogicalRect moved_parent{parent.x - 1024, parent.y + 200, parent.width, parent.height};
    request.anchor.x -= 1024;
    request.anchor.y += 200;
    assert(PreparePopupPositioner(request, moved_parent) == plan);
}

void PartialVisibility()
{
    PopupPositionerRequest request{{-10.25, -5.75, 40, 20}, {200, 100}};
    const LogicalRect parent{0, 0, 100, 100};
    auto plan = PreparePopupPositioner(request, parent);
    assert(plan && (plan->anchor == PopupPositionerRect{0, 0, 30, 15}));

    request.anchor = {99.75, 99.75, 20, 20};
    plan = PreparePopupPositioner(request, parent);
    assert(plan && (plan->anchor == PopupPositionerRect{99, 99, 1, 1}));
    request.anchor = {100, 99, 1, 1};
    assert(!PreparePopupPositioner(request, parent));
    request.anchor = {99, 100, 1, 1};
    assert(!PreparePopupPositioner(request, parent));
    request.anchor = {-50, 10, 50, 20};
    assert(!PreparePopupPositioner(request, parent));
}

void PreferencesAndLimits()
{
    PopupPositionerRequest request{{4, 8, 16, 32}, {8192, 8192}, 256};
    const LogicalRect parent{0, 0, 8192, 8192};
    for (const auto alignment : {PopupHorizontalAlignment::Start, PopupHorizontalAlignment::Center,
                                 PopupHorizontalAlignment::End}) {
        for (const auto preference :
             {PopupVerticalPreference::Below, PopupVerticalPreference::Above}) {
            request.horizontal_alignment = alignment;
            request.vertical_preference = preference;
            const auto plan = PreparePopupPositioner(request, parent);
            assert(plan && plan->horizontal_alignment == alignment);
            assert(plan->vertical_preference == preference);
            assert(plan->width == 8192 && plan->height == 8192 && plan->gap == 256);
        }
    }

    request.desired = {0.01, 0.01};
    request.gap = 0;
    const auto tiny = PreparePopupPositioner(request, parent);
    assert(tiny && tiny->width == 1 && tiny->height == 1 && tiny->gap == 0);
}

void RejectMalformed()
{
    const LogicalRect parent{0, 0, 600, 400};
    const PopupPositionerRequest baseline{{30, 40, 30, 20}, {280, 240}};
    const double bad_values[]{0, -1, 8192.1, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()};
    for (double bad : bad_values) {
        auto request = baseline;
        request.desired.width = bad;
        assert(!PreparePopupPositioner(request, parent));
        request = baseline;
        request.desired.height = bad;
        assert(!PreparePopupPositioner(request, parent));
    }
    for (double bad : {-1.0, 256.1, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        auto request = baseline;
        request.gap = bad;
        assert(!PreparePopupPositioner(request, parent));
    }
    auto request = baseline;
    request.anchor.width = 0;
    assert(!PreparePopupPositioner(request, parent));
    request.anchor = {std::numeric_limits<double>::max(), 0, std::numeric_limits<double>::max(), 5};
    assert(!PreparePopupPositioner(request, parent));
    request = baseline;
    request.horizontal_alignment = static_cast<PopupHorizontalAlignment>(33);
    assert(!PreparePopupPositioner(request, parent));
    request = baseline;
    request.vertical_preference = static_cast<PopupVerticalPreference>(33);
    assert(!PreparePopupPositioner(request, parent));

    assert(!PreparePopupPositioner(baseline, {0.25, 0, 600, 400}));
    assert(!PreparePopupPositioner(baseline, {0, 0, 600.5, 400}));
    assert(!PreparePopupPositioner(baseline, {0, 0, 0, 400}));
    assert(!PreparePopupPositioner(baseline, {0, 0, 8193, 400}));
    assert(!PreparePopupPositioner(
        baseline, {static_cast<double>(std::numeric_limits<std::int32_t>::max()), 0, 600, 400}));
    assert(!PreparePopupPositioner(baseline, {0, 0, 600, std::numeric_limits<double>::infinity()}));
}

void ParentOriginExtremes()
{
    const double origins[]{static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                           static_cast<double>(std::numeric_limits<std::int32_t>::max()) - 8192};
    for (double origin : origins) {
        const LogicalRect parent{origin, origin, 8192, 8192};
        const PopupPositionerRequest request{{origin + 10.25, origin + 20.25, 24.5, 10.5},
                                             {100, 50}};
        const auto plan = PreparePopupPositioner(request, parent);
        assert(plan && (plan->anchor == PopupPositionerRect{10, 20, 25, 11}));
    }
}
} // namespace

int main()
{
    CoordinateSpaces();
    PartialVisibility();
    PreferencesAndLimits();
    RejectMalformed();
    ParentOriginExtremes();
}
