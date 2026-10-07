#pragma once
#include "prism/core/types.hpp"
#include <array>
#include <charconv>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace prism::ipc {
// Preserve the previous stream numeric boundary without constructing JSON text.
// Parsing the formatted number back to double keeps the serializer type-safe.
inline double NumericWireValue(double value, std::chars_format format, int precision)
{
    std::array<char, 384> buffer{};
    const auto formatted =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format, precision);
    if (formatted.ec != std::errc{}) {
        throw std::range_error("IPC numeric formatting failed");
    }

    double result{};
    const auto parsed = std::from_chars(buffer.data(), formatted.ptr, result);
    if (parsed.ec != std::errc{} || parsed.ptr != formatted.ptr) {
        throw std::range_error("IPC numeric conversion failed");
    }
    return result;
}

inline double WireSignificantValue(double value)
{
    return NumericWireValue(value, std::chars_format::general, 6);
}

inline double WireFixedValue(double value, int precision)
{
    return NumericWireValue(value, std::chars_format::fixed, precision);
}

struct RectMessage {
    float x{}, y{}, width{}, height{};
    RectMessage() = default;

    explicit RectMessage(const core::Rect &r) : x(r.x), y(r.y), width(r.width), height(r.height)
    {
    }
};

inline void to_json(nlohmann::json &json, const RectMessage &message)
{
    json = nlohmann::json{{"x", WireSignificantValue(message.x)},
                          {"y", WireSignificantValue(message.y)},
                          {"width", WireSignificantValue(message.width)},
                          {"height", WireSignificantValue(message.height)}};
}

inline void from_json(const nlohmann::json &json, RectMessage &message)
{
    json.at("x").get_to(message.x);
    json.at("y").get_to(message.y);
    json.at("width").get_to(message.width);
    json.at("height").get_to(message.height);
}

struct FractionMessage {
    double width{}, height{};
};

inline void to_json(nlohmann::json &json, const FractionMessage &message)
{
    json = nlohmann::json{{"width", WireSignificantValue(message.width)},
                          {"height", WireSignificantValue(message.height)}};
}

inline void from_json(const nlohmann::json &json, FractionMessage &message)
{
    json.at("width").get_to(message.width);
    json.at("height").get_to(message.height);
}

struct TreeNodeMessage {
    std::int64_t id{};
    std::string type;
    bool focused{};
    RectMessage rect;
    std::optional<FractionMessage> fraction;
    std::optional<std::string> name, app_id, layout;
    std::optional<bool> active, native, visible, fullscreen;
    std::optional<int> active_child_index, pid;
    std::optional<std::uint64_t> instance;
    std::optional<RectMessage> tile_rect, committed_rect;
    std::optional<std::vector<TreeNodeMessage>> nodes;
};

inline void to_json(nlohmann::json &j, const TreeNodeMessage &m)
{
    j = nlohmann::json{{"id", m.id}, {"type", m.type}, {"focused", m.focused}, {"rect", m.rect}};
    if (m.fraction) {
        j["fraction"] = *m.fraction;
    }
    if (m.name) {
        j["name"] = *m.name;
    }
    if (m.app_id) {
        j["app_id"] = *m.app_id;
    }
    if (m.layout) {
        j["layout"] = *m.layout;
    }
    if (m.active) {
        j["active"] = *m.active;
    }
    if (m.native) {
        j["native"] = *m.native;
    }
    if (m.visible) {
        j["visible"] = *m.visible;
    }
    if (m.fullscreen) {
        j["fullscreen"] = *m.fullscreen;
    }
    if (m.active_child_index) {
        j["active_child_index"] = *m.active_child_index;
    }
    if (m.pid) {
        j["pid"] = *m.pid;
    }
    if (m.instance) {
        j["instance"] = *m.instance;
    }
    if (m.tile_rect) {
        j["tile_rect"] = *m.tile_rect;
    }
    if (m.committed_rect) {
        j["committed_rect"] = *m.committed_rect;
    }
    if (m.nodes) {
        j["nodes"] = *m.nodes;
    }
}

inline void from_json(const nlohmann::json &j, TreeNodeMessage &m)
{
    m = {};
    j.at("id").get_to(m.id);
    j.at("type").get_to(m.type);
    j.at("focused").get_to(m.focused);
    j.at("rect").get_to(m.rect);
    if (j.contains("fraction")) {
        m.fraction = j.at("fraction").get<typename decltype(m.fraction)::value_type>();
    }
    if (j.contains("name")) {
        m.name = j.at("name").get<typename decltype(m.name)::value_type>();
    }
    if (j.contains("app_id")) {
        m.app_id = j.at("app_id").get<typename decltype(m.app_id)::value_type>();
    }
    if (j.contains("layout")) {
        m.layout = j.at("layout").get<typename decltype(m.layout)::value_type>();
    }
    if (j.contains("active")) {
        m.active = j.at("active").get<typename decltype(m.active)::value_type>();
    }
    if (j.contains("native")) {
        m.native = j.at("native").get<typename decltype(m.native)::value_type>();
    }
    if (j.contains("visible")) {
        m.visible = j.at("visible").get<typename decltype(m.visible)::value_type>();
    }
    if (j.contains("fullscreen")) {
        m.fullscreen = j.at("fullscreen").get<typename decltype(m.fullscreen)::value_type>();
    }
    if (j.contains("active_child_index")) {
        m.active_child_index =
            j.at("active_child_index").get<typename decltype(m.active_child_index)::value_type>();
    }
    if (j.contains("pid")) {
        m.pid = j.at("pid").get<typename decltype(m.pid)::value_type>();
    }
    if (j.contains("instance")) {
        m.instance = j.at("instance").get<typename decltype(m.instance)::value_type>();
    }
    if (j.contains("tile_rect")) {
        m.tile_rect = j.at("tile_rect").get<typename decltype(m.tile_rect)::value_type>();
    }
    if (j.contains("committed_rect")) {
        m.committed_rect =
            j.at("committed_rect").get<typename decltype(m.committed_rect)::value_type>();
    }
    if (j.contains("nodes")) {
        m.nodes = j.at("nodes").get<typename decltype(m.nodes)::value_type>();
    }
}

struct TreeMessage {
    std::string type{"root"}, active_workspace;
    std::uint64_t focused_id{};
    std::vector<TreeNodeMessage> workspaces;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TreeMessage, type, active_workspace, focused_id, workspaces)

struct CommandReply {
    std::string status;
    std::optional<std::string> message, output, layout, direction, workspace, active_workspace,
        current_theme, id, color_scheme, app, action;
    std::optional<int> width, height, schema_version;
    std::optional<bool> debug_hud, fullscreen;
    std::optional<std::uint64_t> generation;
};

inline void to_json(nlohmann::json &j, const CommandReply &m)
{
    j = nlohmann::json{{"status", m.status}};
    if (m.message) {
        j["message"] = *m.message;
    }
    if (m.output) {
        j["output"] = *m.output;
    }
    if (m.layout) {
        j["layout"] = *m.layout;
    }
    if (m.direction) {
        j["direction"] = *m.direction;
    }
    if (m.workspace) {
        j["workspace"] = *m.workspace;
    }
    if (m.active_workspace) {
        j["active_workspace"] = *m.active_workspace;
    }
    if (m.current_theme) {
        j["current_theme"] = *m.current_theme;
    }
    if (m.id) {
        j["id"] = *m.id;
    }
    if (m.color_scheme) {
        j["color_scheme"] = *m.color_scheme;
    }
    if (m.app) {
        j["app"] = *m.app;
    }
    if (m.action) {
        j["action"] = *m.action;
    }
    if (m.width) {
        j["width"] = *m.width;
    }
    if (m.height) {
        j["height"] = *m.height;
    }
    if (m.schema_version) {
        j["schema_version"] = *m.schema_version;
    }
    if (m.debug_hud) {
        j["debug_hud"] = *m.debug_hud;
    }
    if (m.fullscreen) {
        j["fullscreen"] = *m.fullscreen;
    }
    if (m.generation) {
        j["generation"] = *m.generation;
    }
}

inline void from_json(const nlohmann::json &j, CommandReply &m)
{
    m = {};
    j.at("status").get_to(m.status);
    if (j.contains("message")) {
        m.message = j.at("message").get<typename decltype(m.message)::value_type>();
    }
    if (j.contains("output")) {
        m.output = j.at("output").get<typename decltype(m.output)::value_type>();
    }
    if (j.contains("layout")) {
        m.layout = j.at("layout").get<typename decltype(m.layout)::value_type>();
    }
    if (j.contains("direction")) {
        m.direction = j.at("direction").get<typename decltype(m.direction)::value_type>();
    }
    if (j.contains("workspace")) {
        m.workspace = j.at("workspace").get<typename decltype(m.workspace)::value_type>();
    }
    if (j.contains("active_workspace")) {
        m.active_workspace =
            j.at("active_workspace").get<typename decltype(m.active_workspace)::value_type>();
    }
    if (j.contains("current_theme")) {
        m.current_theme =
            j.at("current_theme").get<typename decltype(m.current_theme)::value_type>();
    }
    if (j.contains("id")) {
        m.id = j.at("id").get<typename decltype(m.id)::value_type>();
    }
    if (j.contains("color_scheme")) {
        m.color_scheme = j.at("color_scheme").get<typename decltype(m.color_scheme)::value_type>();
    }
    if (j.contains("app")) {
        m.app = j.at("app").get<typename decltype(m.app)::value_type>();
    }
    if (j.contains("action")) {
        m.action = j.at("action").get<typename decltype(m.action)::value_type>();
    }
    if (j.contains("width")) {
        m.width = j.at("width").get<typename decltype(m.width)::value_type>();
    }
    if (j.contains("height")) {
        m.height = j.at("height").get<typename decltype(m.height)::value_type>();
    }
    if (j.contains("schema_version")) {
        m.schema_version =
            j.at("schema_version").get<typename decltype(m.schema_version)::value_type>();
    }
    if (j.contains("debug_hud")) {
        m.debug_hud = j.at("debug_hud").get<typename decltype(m.debug_hud)::value_type>();
    }
    if (j.contains("fullscreen")) {
        m.fullscreen = j.at("fullscreen").get<typename decltype(m.fullscreen)::value_type>();
    }
    if (j.contains("generation")) {
        m.generation = j.at("generation").get<typename decltype(m.generation)::value_type>();
    }
}

struct OutputModeMessage {
    int width{}, height{};
    double refresh_hz{};
    bool preferred{}, current{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutputModeMessage, width, height, refresh_hz, preferred, current)

struct OutputMessage {
    std::string name, make, model;
    int width{}, height{}, refresh_mhz{};
    double refresh_hz{}, current_fps{};
    bool adaptive_sync{};
    std::vector<OutputModeMessage> modes;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutputMessage, name, make, model, width, height, refresh_mhz,
                                   refresh_hz, current_fps, adaptive_sync, modes)

struct OutputsReply {
    std::string status{"ok"};
    std::vector<OutputMessage> outputs;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutputsReply, status, outputs)

struct TimingMessage {
    std::size_t samples{};
    double mean_ms{}, p50_ms{}, p95_ms{}, p99_ms{}, max_ms{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TimingMessage, samples, mean_ms, p50_ms, p95_ms, p99_ms, max_ms)

struct PresentationMessage {
    std::string output;
    std::uint64_t presented{}, discarded{};
    TimingMessage interval;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PresentationMessage, output, presented, discarded, interval)

struct ScheduleRequestsMessage {
    std::uint64_t layout{}, effects{}, mode{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScheduleRequestsMessage, layout, effects, mode)

struct SchedulingMessage {
    std::uint64_t frame_events{};
    std::uint64_t idle_skips{};
    std::uint64_t scene_commit_calls{};
    std::uint64_t scene_commit_noops{};
    std::uint64_t output_commits{};
    std::uint64_t output_buffer_commits{};
    std::uint64_t frame_done_dispatches{};
    std::uint64_t needs_frame_events{};
    std::uint64_t damage_events{};
    std::uint64_t surface_commits{};
    std::uint64_t surface_buffer_commits{};
    std::uint64_t surface_callback_commits{};
    std::uint64_t surface_nonvisual_commits{};
    ScheduleRequestsMessage schedule_requests;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SchedulingMessage, frame_events, idle_skips, scene_commit_calls,
                                   scene_commit_noops, output_commits, output_buffer_commits,
                                   frame_done_dispatches, needs_frame_events, damage_events,
                                   surface_commits, surface_buffer_commits,
                                   surface_callback_commits, surface_nonvisual_commits,
                                   schedule_requests)

struct EffectsWorkMessage {
    std::uint64_t update_calls{};
    std::uint64_t skipped_updates{};
    std::uint64_t unsupported_updates{};
    std::uint64_t dirty_transitions{};
    std::uint64_t wake_notifications{};
    std::uint64_t regions_checked{};
    std::uint64_t cache_hits{};
    std::uint64_t cache_misses{};
    std::uint64_t capture_pass_attempts{};
    std::uint64_t capture_passes{};
    std::uint64_t blur_pass_attempts{};
    std::uint64_t blur_passes{};
    std::uint64_t material_pass_attempts{};
    std::uint64_t material_passes{};
    std::uint64_t allocation_attempts{};
    std::uint64_t allocated_buffers{};
    std::uint64_t allocation_failures{};
    std::uint64_t rendered_regions{};
    std::uint64_t rendered_pixels{};
    std::uint64_t failed_regions{};
    std::uint64_t invalid_regions{};
    std::uint64_t removed_regions{};
    std::uint64_t scene_reorders{};
    std::uint64_t dependency_leaves_checked{};
    std::uint64_t dependency_leaves_included{};
    std::uint64_t dependency_leaves_skipped{};
    std::uint64_t content_revisions{};
    std::uint64_t metadata_commits{};
    std::uint64_t damage_history_fallbacks{};
    std::uint64_t partial_damage_cache_hits{};
    std::uint64_t capture_nodes{};
    std::uint64_t mask_builds{};
    std::uint64_t mask_cache_hits{};
    std::uint64_t mask_failures{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
    EffectsWorkMessage, update_calls, skipped_updates, unsupported_updates, dirty_transitions,
    wake_notifications, regions_checked, cache_hits, cache_misses, capture_pass_attempts,
    capture_passes, blur_pass_attempts, blur_passes, material_pass_attempts, material_passes,
    allocation_attempts, allocated_buffers, allocation_failures, rendered_regions, rendered_pixels,
    failed_regions, invalid_regions, removed_regions, scene_reorders, dependency_leaves_checked,
    dependency_leaves_included, dependency_leaves_skipped, content_revisions, metadata_commits,
    damage_history_fallbacks, partial_damage_cache_hits, capture_nodes, mask_builds,
    mask_cache_hits, mask_failures)

struct PerformanceMessage {
    std::size_t sample_capacity{240};
    TimingMessage frame_cpu, effects_cpu, commit_cpu, pointer_event_age;
    std::uint64_t commit_successes{}, commit_failures{}, pointer_events{};
    std::vector<PresentationMessage> presentation;
    SchedulingMessage scheduling;
    EffectsWorkMessage effects_work;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PerformanceMessage, sample_capacity, frame_cpu, effects_cpu,
                                   commit_cpu, commit_successes, commit_failures, pointer_events,
                                   pointer_event_age, presentation, scheduling, effects_work)

struct ThemeStateMessage {
    std::string id;
    std::uint64_t generation{};
    std::string color_scheme;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ThemeStateMessage, id, generation, color_scheme)

struct StatusReply {
    std::string status{"ok"}, version{"Project PrismWM 0.1.0 (wlroots 0.18 Native)"};
    double fps{};
    std::uint64_t frame_count{};
    std::size_t outputs_count{}, windows_count{};
    bool mission_control{}, debug_hud{};
    ThemeStateMessage theme;
    PerformanceMessage performance;
    std::string wayland_socket, ipc_socket;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StatusReply, status, version, fps, frame_count, outputs_count,
                                   windows_count, mission_control, debug_hud, theme, performance,
                                   wayland_socket, ipc_socket)

struct BenchReply {
    std::string status{"ok"}, channel;
    int packets_tested{};
    std::int64_t total_time_us{};
    double avg_latency_ns{}, avg_latency_us{}, throughput_mops{};
    std::string verdict;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BenchReply, status, channel, packets_tested, total_time_us,
                                   avg_latency_ns, avg_latency_us, throughput_mops, verdict)
} // namespace prism::ipc
