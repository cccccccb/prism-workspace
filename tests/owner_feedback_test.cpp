#include "prism/contracts/app_feedback.h"
#include "prism/contracts/owner_feedback.hpp"

#include <array>
#include <cassert>
#include <string>

using namespace prism::contracts;

namespace {
OwnerFeedbackRequest Request()
{
    return {1,
            OwnerFeedbackKind::Info,
            "Saved",
            "The document is ready.\nContinue editing.",
            {{7, "Open"}, {9, "Change location"}},
            4000};
}

void Text()
{
    auto request = Request();
    assert(ValidateOwnerFeedbackRequest(request));
    request.title = "已保存 🙂";
    assert(ValidateOwnerFeedbackRequest(request));
    for (unsigned character = 0; character < 32; ++character) {
        request.title.assign(1, static_cast<char>(character));
        assert(!ValidateOwnerFeedbackRequest(request));
        request = Request();
        request.message.assign(1, static_cast<char>(character));
        assert(ValidateOwnerFeedbackRequest(request) == (character == '\n'));
    }
    for (unsigned character = 0x80; character <= 0x9f; ++character) {
        request = Request();
        request.message = std::string{char(0xc2), static_cast<char>(character)};
        assert(!ValidateOwnerFeedbackRequest(request));
    }
    for (const auto &invalid :
         {std::string("\x7f"), std::string("\xc0\xaf"), std::string("\xe0\x80\xaf"),
          std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xe2\x80\xa8"),
          std::string("\xe2\x80\xa9"), std::string("\xf0\x9f"), std::string("\x80")}) {
        request = Request();
        request.message = invalid;
        assert(!ValidateOwnerFeedbackRequest(request));
    }
    request = Request();
    request.title.assign(256, 'x');
    request.message.assign(2048, 'x');
    request.actions[0].label.assign(48, 'x');
    assert(ValidateOwnerFeedbackRequest(request));
    request.title += 'x';
    assert(!ValidateOwnerFeedbackRequest(request));
    request.title.pop_back();
    request.message += 'x';
    assert(!ValidateOwnerFeedbackRequest(request));
    request.message.pop_back();
    request.actions[0].label += 'x';
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.title.clear();
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.message.clear();
    request.actions.clear();
    assert(ValidateOwnerFeedbackRequest(request));
}

void Values()
{
    auto request = Request();
    static_assert(PRISM_FEEDBACK_CAP_OWNER_V1 == kOwnerFeedbackCapability);
    request.request_id = 0;
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.kind = static_cast<OwnerFeedbackKind>(3);
    assert(!ValidateOwnerFeedbackRequest(request));
    for (auto kind : {OwnerFeedbackKind::Info, OwnerFeedbackKind::Success}) {
        for (const auto duration : std::array<unsigned, 7>{0, 1, 999, 1000, 4000, 30000, 30001}) {
            request = Request();
            request.kind = kind;
            request.duration_ms = duration;
            assert(ValidateOwnerFeedbackRequest(request) ==
                   (!duration || (duration >= 1000 && duration <= 30000)));
        }
    }
    request = Request();
    request.kind = OwnerFeedbackKind::Error;
    request.duration_ms = 0;
    assert(ValidateOwnerFeedbackRequest(request));
    request.duration_ms = 4000;
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.actions[1].id = 7;
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.actions[1].id = 0;
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.actions[0].label.clear();
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    request.actions.push_back({10, "Another"});
    assert(!ValidateOwnerFeedbackRequest(request));
    request = Request();
    assert(ValidateOwnerFeedbackAction({1, 7}, request));
    assert(ValidateOwnerFeedbackAction({1, 9}, request));
    assert(!ValidateOwnerFeedbackAction({0, 7}, request));
    assert(!ValidateOwnerFeedbackAction({2, 7}, request));
    assert(!ValidateOwnerFeedbackAction({1, 0}, request));
    assert(!ValidateOwnerFeedbackAction({1, 10}, request));
    request.actions.clear();
    assert(!ValidateOwnerFeedbackAction({1, 7}, request));
}
} // namespace

int main()
{
    Text();
    Values();
}
