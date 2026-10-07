#include "notepad.hpp"
#include <algorithm>

namespace prism::notepad {
void Notepad::String(std::string_view key, std::string_view text)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_STRING_V1;
    value.as.string = {text.data(), text.size()};
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Number(std::string_view key, double number)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_NUMBER_V1;
    value.as.number = number;
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Boolean(std::string_view key, bool state)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_BOOL_V1;
    value.as.boolean = state;
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Publish()
{
    const bool normal = !list_;
    const bool available = !busy_ && !request_id_ && !close_id_ && closing_ == NoDocument;
    const auto total = std::count_if(documents_.begin(), documents_.end(),
                                     [](const Document &doc) { return doc.used; });
    tab_page_ = std::min(tab_page_, static_cast<std::size_t>((total - 1) / 2));
    std::size_t count = 0;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        const auto &document = documents_[i];
        const auto suffix = std::to_string(i);
        Boolean("tab_" + suffix, document.used && count / 2 == tab_page_ && normal);
        count += document.used;
        Boolean("active_" + suffix, document.used && i == active_ && normal);
        Boolean("used_" + suffix, document.used);
        Boolean("selected_" + suffix, document.used && i == active_);
        String("text_" + suffix, document.text);
        String("label_" + suffix, (document.Dirty() ? "● " : "") + document.title);
    }

    const auto &document = documents_[active_];
    String("title", (document.Dirty() ? "● " : "") + document.title);
    String("path", document.path);
    String("status", message_);
    String("details",
           std::to_string(count) + "/8 documents · " + std::to_string(document.text.size()) +
               " bytes · " +
               std::to_string(1 + std::count(document.text.begin(), document.text.end(), '\n')) +
               " lines" + (document.Dirty() ? " · Unsaved" : ""));
    String("summary",
           std::to_string(1 + std::count(document.text.begin(), document.text.end(), '\n')) +
               " lines · " + (document.Dirty() ? "Unsaved" : "UTF-8"));
    Boolean("normal", normal);
    Boolean("available", available);
    Boolean("editable", available || (busy_ && saving_));
    Boolean("pages", total > 2);
    Boolean("documents", list_);
    Boolean("busy", busy_);
    Number("list_opacity", list_ ? 1 : 0);
    Number("list_offset", list_ ? 0 : 6);
    Number("activity_opacity", busy_ ? 1 : 0);
}
} // namespace prism::notepad
