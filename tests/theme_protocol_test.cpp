#include "prism/launch/control_protocol.hpp"
#include "prism/launch/worker_protocol.hpp"
#include "prism/theme/compiler.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace prism;

constexpr std::size_t kExpandedTokenCount = 129;
constexpr std::size_t kTokenLimit = 256;

template <class F> void Reject(F f)
{
    bool rejected = false;
    try {
        f();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void CheckTokenBudget(const contracts::ThemeSnapshot &base)
{
    auto expanded = base;
    expanded.numbers.clear();
    expanded.colors.clear();

    for (std::size_t i = 0; i < kTokenLimit; ++i) {
        expanded.numbers.push_back({"number_" + std::to_string(i), 1});
        if (i + 1 == kExpandedTokenCount || i + 1 == kTokenLimit) {
            assert(contracts::DecodeTheme(contracts::EncodeTheme(expanded)) == expanded);
        }
    }
    expanded.numbers.push_back({"over_budget", 1});
    Reject([&] { contracts::ValidateTheme(expanded); });
    Reject([&] { contracts::EncodeTheme(expanded); });

    expanded.numbers.clear();
    for (std::size_t i = 0; i < kTokenLimit; ++i) {
        expanded.colors.push_back({"color_" + std::to_string(i), {1, 2, 3, 255}});
        if (i + 1 == kExpandedTokenCount || i + 1 == kTokenLimit) {
            assert(contracts::DecodeTheme(contracts::EncodeTheme(expanded)) == expanded);
        }
    }
    expanded.colors.push_back({"over_budget", {1, 2, 3, 255}});
    Reject([&] { contracts::ValidateTheme(expanded); });
    Reject([&] { contracts::EncodeTheme(expanded); });

    expanded.colors.resize(128);
    for (std::size_t i = 0; i < 128; ++i) {
        expanded.numbers.push_back({"number_" + std::to_string(i), 1});
    }
    assert(contracts::DecodeTheme(contracts::EncodeTheme(expanded)) == expanded);
    expanded.colors.push_back({"over_budget", {1, 2, 3, 255}});
    Reject([&] { contracts::ValidateTheme(expanded); });
    Reject([&] { contracts::EncodeTheme(expanded); });
    expanded.colors.pop_back();

    // An excessive count must fail before parsing entries or allocating them.
    auto bytes = contracts::EncodeTheme(expanded);
    const auto numbers_offset = 12 + 2 + expanded.id.size() + 2 + expanded.name.size();
    auto colors_offset = numbers_offset + 2;
    for (const auto &number : expanded.numbers) {
        colors_offset += 2 + number.name.size() + 8;
    }
    bytes[colors_offset] = 0;
    bytes[colors_offset + 1] = kExpandedTokenCount;
    bool rejected = false;
    try {
        contracts::DecodeTheme(bytes);
    } catch (const std::invalid_argument &error) {
        rejected = std::string_view(error.what()) == "Too many theme colors";
    }
    assert(rejected);

    bytes = contracts::EncodeTheme(expanded);
    bytes[numbers_offset] = 1;
    bytes[numbers_offset + 1] = 1;
    rejected = false;
    try {
        contracts::DecodeTheme(bytes);
    } catch (const std::invalid_argument &error) {
        rejected = std::string_view(error.what()) == "Too many theme numbers";
    }
    assert(rejected);
}

int main()
{
    auto t = theme::LoadTheme(PRISM_SOURCE_THEMES, "glass", 41);
    CheckTokenBudget(t);
    const auto encoded = contracts::EncodeTheme(t);
    assert(contracts::DecodeTheme(encoded) == t);
    assert(t.schema_version == 3 && t.color_scheme == "dark");
    auto light = theme::LoadTheme(PRISM_SOURCE_THEMES, "glass", 42, "light");
    assert(contracts::DecodeTheme(contracts::EncodeTheme(light)) == light);
    assert(contracts::ThemeColorValue(light, "text") != contracts::ThemeColorValue(t, "text"));
    // Schema 1 has no new tail. All bytes after its schema header retain the
    // historical encoding, and a new decoder supplies the dark default.
    auto v2 = t;
    v2.schema_version = 2;
    v2.motion = {};
    const auto v2_bytes = contracts::EncodeTheme(v2);
    assert(contracts::DecodeTheme(v2_bytes) == v2);
    auto legacy = v2;
    legacy.schema_version = 1;
    const auto legacy_bytes = contracts::EncodeTheme(legacy);
    assert(legacy_bytes.size() + 6 == v2_bytes.size());
    assert(std::equal(legacy_bytes.begin() + 4, legacy_bytes.end(), v2_bytes.begin() + 4));
    assert(contracts::DecodeTheme(legacy_bytes) == legacy);
    auto invalid_legacy = legacy;
    invalid_legacy.color_scheme = "light";
    Reject([&] { contracts::EncodeTheme(invalid_legacy); });
    auto unknown_scheme = t;
    unknown_scheme.color_scheme = "auto";
    Reject([&] { contracts::EncodeTheme(unknown_scheme); });
    auto short_scheme = v2_bytes;
    short_scheme.pop_back();
    Reject([&] { contracts::DecodeTheme(short_scheme); });
    auto bad_scheme = v2_bytes;
    bad_scheme.back() = 'x';
    Reject([&] { contracts::DecodeTheme(bad_scheme); });
    for (auto size : {0u, 1u, 12u, 64u}) {
        Reject([&] { contracts::DecodeTheme(std::span(encoded).first(size)); });
    }
    auto bytes = encoded;
    bytes.push_back(0);
    Reject([&] { contracts::DecodeTheme(bytes); });
    auto invalid = t;
    invalid.numbers.push_back(invalid.numbers.front());
    Reject([&] { contracts::EncodeTheme(invalid); });
    invalid = t;
    invalid.materials.front().backdrop_blur = 49;
    Reject([&] { contracts::EncodeTheme(invalid); });
    invalid = t;
    invalid.layout.outer_gap = std::numeric_limits<double>::quiet_NaN();
    Reject([&] { contracts::EncodeTheme(invalid); });
    invalid = t;
    invalid.id = "../glass";
    Reject([&] { contracts::EncodeTheme(invalid); });
    launch::ControlMessage m;
    m.type = launch::ControlType::InstallTheme;
    m.permit.session = 17;
    m.theme = t;
    auto c = launch::EncodeControl(m);
    assert(launch::ControlFrameSize(c) == c.size());
    assert(launch::DecodeControl(c).theme == t);
    c.push_back(0);
    Reject([&] { launch::DecodeControl(c); });
    m.type = launch::ControlType::ThemeApplied;
    m.theme_applied = {41, true, {}};
    assert(launch::DecodeControl(launch::EncodeControl(m)).theme_applied.generation == 41);
    auto worker = launch::DecodeWorker(launch::EncodeWorker(t));
    assert(std::get<contracts::ThemeSnapshot>(worker) == t);
    worker = launch::DecodeWorker(
        launch::EncodeWorker(contracts::ThemeApplied{41, false, "Unsupported material"}));
    assert(!std::get<contracts::ThemeApplied>(worker).success);
    auto query = std::get<contracts::ThemeRequest>(
        launch::DecodeMessage(launch::EncodeMessage(contracts::ThemeRequest{8, {}})));
    assert(query.request == 8 && query.id.empty());
    assert(query.color_scheme.empty());
    const std::vector<std::uint8_t> old_query{0, 0, 0, 0, 0, 0, 0, 8, 0, 0};
    assert(contracts::EncodeThemeRequest({8, {}}) == old_query);
    assert(contracts::DecodeThemeRequest(old_query).color_scheme.empty());
    const auto light_request = contracts::ThemeRequest{9, {}, "light"};
    const auto request_bytes = contracts::EncodeThemeRequest(light_request);
    assert(contracts::DecodeThemeRequest(request_bytes).color_scheme == "light");
    auto extension = request_bytes;
    extension[10] = 2;
    Reject([&] { contracts::DecodeThemeRequest(extension); });
    extension = request_bytes;
    extension[12] = 6;
    Reject([&] { contracts::DecodeThemeRequest(extension); });
    extension = request_bytes;
    extension.push_back(0);
    Reject([&] { contracts::DecodeThemeRequest(extension); });
    extension = request_bytes;
    extension.back() = 'x';
    Reject([&] { contracts::DecodeThemeRequest(extension); });
    Reject([&] { contracts::EncodeThemeRequest({9, {}, "auto"}); });
    auto scheme_worker = launch::DecodeWorker(launch::EncodeWorker(light_request));
    assert(std::get<contracts::ThemeRequest>(scheme_worker).color_scheme == "light");
    auto scheme_client =
        launch::DecodeMessage(launch::EncodeMessage(contracts::ThemeRequest{10, "square", "dark"}));
    const auto &both = std::get<contracts::ThemeRequest>(scheme_client);
    assert(both.id == "square" && both.color_scheme == "dark");
    contracts::ThemeEvent event{8, 41, contracts::ThemeStatus::Applied, t.id, t.name, "Installed"};
    auto result =
        std::get<contracts::ThemeEvent>(launch::DecodeMessage(launch::EncodeMessage(event)));
    assert(result.request == 8 && result.generation == 41 && result.id == t.id);
    assert(result.color_scheme == "dark");
    const auto dark_event_bytes = contracts::EncodeThemeEvent(event);
    auto light_event = event;
    light_event.color_scheme = "light";
    const auto light_event_bytes = contracts::EncodeThemeEvent(light_event);
    assert(light_event_bytes.size() == dark_event_bytes.size() + 8);
    assert(std::equal(dark_event_bytes.begin(), dark_event_bytes.end(), light_event_bytes.begin()));
    assert(contracts::DecodeThemeEvent(dark_event_bytes).color_scheme == "dark");
    assert(contracts::DecodeThemeEvent(light_event_bytes).color_scheme == "light");
    auto bad_event = light_event_bytes;
    bad_event[dark_event_bytes.size()] = 2;
    Reject([&] { contracts::DecodeThemeEvent(bad_event); });
    bad_event = light_event_bytes;
    bad_event.push_back(0);
    Reject([&] { contracts::DecodeThemeEvent(bad_event); });
    auto scheme_event = launch::DecodeMessage(launch::EncodeMessage(light_event));
    assert(std::get<contracts::ThemeEvent>(scheme_event).color_scheme == "light");
    auto light_control = m;
    light_control.type = launch::ControlType::InstallTheme;
    light_control.theme = light;
    assert(launch::DecodeControl(launch::EncodeControl(light_control)).theme == light);
    auto frame = launch::EncodeMessage(contracts::ThemeRequest{8, "square"});
    frame[19] = 9;
    Reject([&] { launch::DecodeMessage(frame); });
    Reject([&] { launch::EncodeMessage(contracts::ThemeRequest{9, "/tmp/theme"}); });
    std::cout
        << "Theme snapshot bounds, round trips, identity and typed PRL/PRW/PWC extensions passed\n";
}
