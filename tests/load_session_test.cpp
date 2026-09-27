#include "prism/runtime/load_session.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace prism::runtime;
using namespace std::chrono_literals;

class Sources {
public:
    Sources()
    {
        char path[] = "/tmp/prism-load-session-XXXXXX";
        const auto result = mkdtemp(path);
        if (!result) {
            throw std::runtime_error("Cannot create source directory");
        }
        directory = result;
    }

    ~Sources()
    {
        std::filesystem::remove_all(directory);
    }

    std::string Write(std::string_view name, std::string_view text)
    {
        auto path = directory / name;
        std::ofstream output(path, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        assert(output.good());
        return path.string();
    }

    std::filesystem::path directory;
};

LoadRequest Request(std::string path, std::uint64_t generation)
{
    return {{71, generation},
            std::move(path),
            {"master-" + std::to_string(generation), "ui/master.prism", "revision-7"}};
}

void WaitReadable(const LoadSession &session)
{
    pollfd ready{session.Fd(), POLLIN, 0};
    assert(poll(&ready, 1, 5000) == 1);
    assert(ready.revents == POLLIN);
}

LoadCompletion Take(LoadSession &session)
{
    WaitReadable(session);
    auto completion = session.TakeCompletion();
    assert(completion);
    return std::move(*completion);
}

void CheckFailure(const LoadCompletion &result, LoadStage stage, UiLoadId load)
{
    assert(result.load == load);
    assert(!result.prepared);
    assert(result.diagnostic);
    assert(result.diagnostic->stage == stage);
    assert(!result.diagnostic->message.empty());
    assert(result.source.component_id == result.diagnostic->source.component_id);
    assert(result.source.source_path == result.diagnostic->source.source_path);
    assert(result.source.source_version == result.diagnostic->source.source_version);
}

class PrepareBarrier {
public:
    PreparedComponent Compile(std::string_view text, ComponentSource source, std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        worker_ = std::this_thread::get_id();
        const auto ordinal = ++entered_;
        changed_.notify_all();
        const bool released =
            changed_.wait(lock, stop, [this, ordinal] { return releases_ >= ordinal; });
        if (!released) {
            throw LoadFailure({LoadStage::Cancelled, source, 0, "Barrier compilation cancelled"});
        }
        lock.unlock();

        auto result = PrepareComponent(text, std::move(source));
        lock.lock();
        ++compiled_;
        changed_.notify_all();
        return result;
    }

    void AwaitEntered(unsigned count)
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, [this, count] { return entered_ >= count; }));
        assert(worker_ != std::this_thread::get_id());
    }

    void AwaitCompiled(unsigned count)
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, [this, count] { return compiled_ >= count; }));
    }

    void Release(unsigned count)
    {
        std::lock_guard lock(mutex_);
        releases_ = count;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    unsigned entered_{};
    unsigned compiled_{};
    unsigned releases_{};
    std::thread::id worker_;
};

void OwningPurePreparation(Sources &files)
{
    LoadSession session;
    assert(!session.TakeCompletion());
    auto request =
        Request(files.Write("images.prism", "Card { Image(\"cover.png\") Text($label) }"), 1);
    assert(session.Submit(request) == LoadSubmitResult::Accepted);
    request.path.clear();
    request.source = {};
    auto result = Take(session);
    assert(result.prepared && !result.diagnostic);
    assert(result.load == (UiLoadId{71, 1}));
    assert(result.source.component_id == "master-1");
    assert(result.prepared->Source().source_version == "revision-7");
    assert(result.prepared->Images().size() == 1);
    assert(result.prepared->Images()[0].uri == "cover.png");
    assert(result.prepared->NodeCount() == 3);
    assert(!session.TakeCompletion());

    auto invalid = Request("", 2);
    assert(session.Submit(invalid) == LoadSubmitResult::Invalid);
    invalid = Request(files.Write("valid.prism", "Text(\"valid\")"), 2);
    invalid.load = {};
    assert(session.Submit(invalid) == LoadSubmitResult::Invalid);
    invalid.load = {71, 2};
    invalid.path.push_back('\0');
    invalid.path += "suffix";
    assert(session.Submit(invalid) == LoadSubmitResult::Invalid);
}

void ReadAndCompileFailures(Sources &files)
{
    LoadSession session;
    const auto fifo = files.directory / "blocked-fifo";
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    const std::string read_failures[] = {
        fifo.string(), files.directory.string(), (files.directory / "missing.prism").string(),
        files.Write("empty.prism", ""),
        files.Write("large.prism", std::string(1024 * 1024 + 1, 'x'))};
    std::uint64_t generation = 10;
    for (const auto &path : read_failures) {
        auto request = Request(path, generation++);
        assert(session.Submit(request) == LoadSubmitResult::Accepted);
        CheckFailure(Take(session), LoadStage::Read, request.load);
    }

    auto syntax =
        Request(files.Write("syntax.prism", "Card {\nText(\"unterminated)"), generation++);
    assert(session.Submit(syntax) == LoadSubmitResult::Accepted);
    const auto syntax_error = Take(session);
    CheckFailure(syntax_error, LoadStage::Syntax, syntax.load);
    assert(syntax_error.diagnostic->line == 2);

    auto semantic =
        Request(files.Write("semantic.prism", "Card {\nText(\"bad\",mystery:1)\n}"), generation++);
    assert(session.Submit(semantic) == LoadSubmitResult::Accepted);
    const auto semantic_error = Take(session);
    CheckFailure(semantic_error, LoadStage::Semantic, semantic.load);
    assert(semantic_error.diagnostic->line == 2);
}

void BoundedRequestsAndHeldResults(Sources &files)
{
    PrepareBarrier barrier;
    LoadSession session(std::bind_front(&PrepareBarrier::Compile, &barrier));
    const auto path = files.Write("bounded.prism", "Text(\"bounded\")");
    assert(session.Submit(Request(path, 30)) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(1);
    assert(session.Submit(Request(path, 31)) == LoadSubmitResult::Accepted);
    assert(session.Submit(Request(path, 32)) == LoadSubmitResult::Busy);

    barrier.Release(1);
    barrier.AwaitEntered(2);
    WaitReadable(session);
    barrier.Release(2);
    barrier.AwaitCompiled(2);
    // One published result and one worker-local result are both still counted.
    assert(session.Submit(Request(path, 32)) == LoadSubmitResult::Busy);
    auto first = Take(session);
    assert(first.prepared && first.load.generation == 30);
    auto second = Take(session);
    assert(second.prepared && second.load.generation == 31);

    assert(session.Submit(Request(path, 32)) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(3);
    barrier.Release(3);
    assert(Take(session).prepared);
}

void CancellationAndReuse(Sources &files)
{
    PrepareBarrier barrier;
    LoadSession session(std::bind_front(&PrepareBarrier::Compile, &barrier));
    const auto path = files.Write("cancelled.prism", "Text(\"cancelled\")");
    const auto first = Request(path, 40);
    const auto queued = Request(path, 41);
    assert(session.Submit(first) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(1);
    assert(session.Submit(queued) == LoadSubmitResult::Accepted);
    session.Cancel(queued.load);
    session.Cancel(first.load);
    CheckFailure(Take(session), LoadStage::Cancelled, first.load);
    CheckFailure(Take(session), LoadStage::Cancelled, queued.load);

    auto completed = Request(path, 42);
    assert(session.Submit(completed) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(2);
    barrier.Release(2);
    WaitReadable(session);
    session.Cancel(completed.load);
    CheckFailure(Take(session), LoadStage::Cancelled, completed.load);

    auto final = Request(path, 43);
    assert(session.Submit(final) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(3);
    barrier.Release(3);
    assert(Take(session).prepared);
}

void StopJoinsAndIsIdempotent(Sources &files)
{
    PrepareBarrier barrier;
    LoadSession session(std::bind_front(&PrepareBarrier::Compile, &barrier));
    const auto path = files.Write("stop.prism", "Text(\"stop\")");
    assert(session.Submit(Request(path, 50)) == LoadSubmitResult::Accepted);
    barrier.AwaitEntered(1);
    assert(session.Submit(Request(path, 51)) == LoadSubmitResult::Accepted);
    session.Stop();
    session.Stop();
    assert(!session.TakeCompletion());
    assert(session.Submit(Request(path, 52)) == LoadSubmitResult::Closed);

    // Destruction also cancels and joins an active compiler adapter.
    {
        LoadSession automatic(std::bind_front(&PrepareBarrier::Compile, &barrier));
        assert(automatic.Submit(Request(path, 53)) == LoadSubmitResult::Accepted);
        barrier.AwaitEntered(2);
    }
}
} // namespace

int main()
{
    Sources files;
    OwningPurePreparation(files);
    ReadAndCompileFailures(files);
    BoundedRequestsAndHeldResults(files);
    CancellationAndReuse(files);
    StopJoinsAndIsIdempotent(files);
    std::cout << "load_session_test: passed\n";
}
