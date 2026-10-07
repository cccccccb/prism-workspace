#include "popup_backdrop_barrier_p.hpp"

#include <cassert>
#include <cstdio>
#include <limits>
#include <memory>

namespace {
using namespace prism;

struct BarrierFixture {
    runtime::UiLoadId ui{41, 3};
    runtime::PopupSurfaceIdentity child{7, 19, 19, 2, 11};
    int parent_configure{4};
    runtime::FramePacket metadata, pixels;

    BarrierFixture()
    {
        auto clean = std::make_shared<contracts::DisplayList>();
        clean->commands.push_back(contracts::FillRect{{0, 0, 320, 200}, {25, 42, 59, 255}});
        pixels.ui = ui;
        pixels.sequence = 20;
        pixels.configure_count = parent_configure;
        pixels.buffer_size = {320, 200};
        pixels.scale = 1;
        pixels.resource_epoch = 8;
        pixels.display_list = std::move(clean);

        metadata = pixels;
        metadata.sequence = 21;
        metadata.popup_surface_excluded = child;
        auto request = std::make_shared<runtime::PopupSurfaceRequest>();
        request->parent_configure_generation = parent_configure;
        metadata.popup_surface_request = std::move(request);
    }

    bool Ready() const
    {
        return sdk::CanEnablePopupBackdrop(metadata, &pixels, child, ui, parent_configure);
    }
};

void FirstPixelAndCheckedBaseline()
{
    BarrierFixture fixture;
    assert(fixture.Ready());
    assert(!sdk::CanEnablePopupBackdrop(fixture.metadata, nullptr, fixture.child, fixture.ui,
                                        fixture.parent_configure));

    // The root previously included this panel. Merely publishing a clean
    // packet, or successfully committing child pixels, cannot retire it.
    auto fallback = std::make_shared<contracts::DisplayList>(*fixture.pixels.display_list);
    fallback->commands.push_back(
        contracts::FillRoundedRect{{40, 60, 160, 80}, 12, {150, 170, 190, 120}});
    fixture.pixels.display_list = std::move(fallback);
    assert(!fixture.Ready());
    fixture.pixels.display_list = fixture.metadata.display_list;
    assert(fixture.Ready());

    // Replacing root metadata by an identical State/None needs no new pixels.
    // A later child metadata adoption does not invalidate the clean root.
    fixture.metadata.sequence += 2;
    fixture.child.submission_sequence += 3;
    assert(fixture.Ready());
    fixture.metadata.popup_surface_excluded.submission_sequence =
        fixture.child.submission_sequence + 1;
    assert(!fixture.Ready());
    fixture.metadata.popup_surface_excluded.submission_sequence = 0;
    assert(!fixture.Ready());
}

void RetiredChildAndUi()
{
    {
        BarrierFixture fixture;
        ++fixture.metadata.popup_surface_excluded.worker;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.metadata.popup_surface_excluded.target;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.metadata.popup_surface_excluded.lifetime;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.metadata.popup_surface_excluded.configure_generation;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.child.submission_sequence = 0;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.child.target = 0;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        --fixture.metadata.ui.generation;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        --fixture.pixels.ui.generation;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.ui.owner;
        assert(!fixture.Ready());
    }
}

void ParentConfigureAndResources()
{
    {
        BarrierFixture fixture;
        --fixture.metadata.configure_count;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.metadata.popup_surface_request.reset();
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        auto request =
            std::make_shared<runtime::PopupSurfaceRequest>(*fixture.metadata.popup_surface_request);
        --request->parent_configure_generation;
        fixture.metadata.popup_surface_request = std::move(request);
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.parent_configure = 0;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        // The checked metadata ACK can change parent configure while retaining
        // the same already successful pixels. Old configure alone is no fault.
        --fixture.pixels.configure_count;
        assert(fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.metadata.resource_epoch;
        assert(!fixture.Ready());
        fixture.pixels.resource_epoch = fixture.metadata.resource_epoch;
        assert(fixture.Ready());
    }
    {
        BarrierFixture fixture;
        ++fixture.metadata.buffer_size.width;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        --fixture.metadata.buffer_size.height;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.metadata.scale = 2;
        assert(!fixture.Ready());
    }
}

void InvalidPixelBaselines()
{
    {
        BarrierFixture fixture;
        fixture.metadata.display_list.reset();
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.pixels.display_list.reset();
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.metadata.buffer_size.width = fixture.pixels.buffer_size.width = 0;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.metadata.scale = fixture.pixels.scale = 0;
        assert(!fixture.Ready());
    }
    {
        BarrierFixture fixture;
        fixture.metadata.scale = fixture.pixels.scale = std::numeric_limits<double>::infinity();
        assert(!fixture.Ready());
    }
}
} // namespace

int main()
{
    FirstPixelAndCheckedBaseline();
    RetiredChildAndUi();
    ParentConfigureAndResources();
    InvalidPixelBaselines();
    std::puts("Popup backdrop barrier: actual clean pixels, checked metadata and identity gates "
              "passed");
}
