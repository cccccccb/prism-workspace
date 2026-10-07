#pragma once

#include "prism/runtime/file_task_model.hpp"

#include <stop_token>

namespace prism::runtime {

struct FileTaskWork {
    std::uint64_t generation{};
    FileTaskOperation operation{FileTaskOperation::Directory};
    contracts::OwnerTaskKind kind{contracts::OwnerTaskKind::OpenFile};
    std::string directory, name;
    std::vector<std::string> extensions;
    bool show_hidden{};

    std::shared_ptr<const TaskOutput> operator()(std::stop_token stop) const;
};

struct FileTaskModel::Impl {
    explicit Impl(std::shared_ptr<TaskScheduler> value);
    std::uint64_t Start(FileTaskWork work);
    bool SubmitPending();
    void Complete(TaskCompletion completion);

    std::shared_ptr<TaskScheduler> scheduler;
    std::shared_ptr<TaskChannel> channel;
    std::optional<FileTaskWork> pending;
    std::shared_ptr<const FileTaskResult> result;
    std::uint64_t generation{};
    FileTaskOperation operation{FileTaskOperation::Directory};
    bool active{}, stopped{};
};

} // namespace prism::runtime
