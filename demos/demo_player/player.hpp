#pragma once

#include "catalogue.hpp"
#include <array>
#include <string>
#include <string_view>

namespace prism::music {
class Player {
public:
    Player(const PrismHostApiV1 *, std::string assets_root);
    bool Initialize();
    void Action(std::string_view);
    void Tick();
    void Completed(const PrismWorkCompletionV1 &);

private:
    bool Load();
    bool PublishPage();
    bool PublishTrack();
    bool PublishRows();
    bool Progress();
    bool Error(std::string_view);
    bool Text(std::string_view, std::string_view);
    bool Boolean(std::string_view, bool);
    bool Number(std::string_view, double);
    bool Schedule();
    void Select(std::size_t);
    static std::string Time(std::uint32_t);

    const PrismHostApiV1 *host_;
    std::string assets_root_;
    Catalogue catalogue_;
    std::array<std::size_t, MaxTracks> rows_{};
    std::size_t row_count_{}, track_{};
    bool pending_{}, playing_{}, library_{}, favorites_{}, ready_{}, catalogue_valid_{};
    double progress_{0.42};
};
} // namespace prism::music
