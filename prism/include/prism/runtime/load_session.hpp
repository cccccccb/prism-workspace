#pragma once

#include "prism/runtime/prepared_component.hpp"
#include "prism/runtime/ui_load.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace prism::runtime {

struct LoadRequest {
    UiLoadId load;
    std::string path;
    ComponentSource source;
};

struct LoadTimings {
    std::uint64_t read_us{};
    std::uint64_t prepare_us{};
};

struct LoadCompletion {
    UiLoadId load;
    ComponentSource source;
    std::optional<PreparedComponent> prepared;
    std::optional<LoadDiagnostic> diagnostic;
    LoadTimings timings;
};

enum class LoadSubmitResult { Accepted, Busy, Closed, Invalid };

// An adapter must be pure CPU compilation, own its state and obey cancellation.
// It must never retain source views or refer to live frontend/platform objects.
using PrepareFunction =
    std::function<PreparedComponent(std::string_view, ComponentSource, std::stop_token)>;

class LoadSession {
public:
    // The default adapter calls PrepareComponent. No thread starts until Submit.
    explicit LoadSession(PrepareFunction prepare = {});
    ~LoadSession();
    LoadSession(const LoadSession &) = delete;
    LoadSession &operator=(const LoadSession &) = delete;
    LoadSession(LoadSession &&) = delete;
    LoadSession &operator=(LoadSession &&) = delete;

    // At most two accepted, unconsumed requests: active, queued or completed.
    // Calls and completion consumption belong to one owner thread.
    LoadSubmitResult Submit(LoadRequest request);
    void Cancel(UiLoadId load);
    // Cancels all work, drops outstanding results and joins. Safe to repeat.
    void Stop();
    int Fd() const noexcept;
    // Drains only this session's notification; no caller-owned FD is consumed.
    std::optional<LoadCompletion> TakeCompletion();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::runtime
