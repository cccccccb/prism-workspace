#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/text_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace prism::runtime {
namespace {
double TextWidth(std::string_view text, double font_size, const ShapeText &shape)
{
    const auto metrics = shape(text, font_size);
    if (!std::isfinite(metrics.width) || !std::isfinite(metrics.height) || metrics.width < 0 ||
        metrics.height < 0) {
        throw std::invalid_argument("Invalid owner task shaping metrics");
    }
    return metrics.width;
}

void WrapParagraph(std::string_view text, double width, double font_size, const ShapeText &shape,
                   std::string &output)
{
    std::vector<std::size_t> offsets{0};
    offsets.reserve(text.size() + 1);
    while (offsets.back() < text.size()) {
        offsets.push_back(TextBuffer::Next(text, offsets.back()));
    }

    std::size_t start = 0;
    const auto end = offsets.size() - 1;
    while (start < end) {
        std::size_t low = start + 1;
        std::size_t high = end;
        std::size_t fit = start;
        while (low <= high) {
            const auto candidate = low + (high - low) / 2;
            const auto part = text.substr(offsets[start], offsets[candidate] - offsets[start]);
            if (TextWidth(part, font_size, shape) <= width) {
                fit = candidate;
                low = candidate + 1;
            } else {
                high = candidate - 1;
            }
        }
        // A scalar wider than the viewport stays intact. It is never dropped,
        // split into invalid UTF-8 or retried forever at the same offset.
        fit = std::max(fit, start + 1);
        if (fit < end) {
            const auto space = text.rfind(' ', offsets[fit] - 1);
            if (space != std::string_view::npos && space > offsets[start]) {
                fit = std::lower_bound(offsets.begin() + start + 1, offsets.begin() + fit + 1,
                                       space + 1) -
                      offsets.begin();
            }
        }

        output.append(text.substr(offsets[start], offsets[fit] - offsets[start]));
        start = fit;
        if (start < end) {
            output.push_back('\n');
        }
    }
}
} // namespace

std::string WrapOwnerTaskText(std::string_view text, double width, double font_size,
                              const ShapeText &shape)
{
    if (!std::isfinite(width) || width <= 0 || !std::isfinite(font_size) || font_size <= 0 ||
        !shape || text.size() > contracts::kMaxOwnerTaskMessageBytes || !TextBuffer::Valid(text) ||
        text.find('\r') != std::string_view::npos) {
        throw std::invalid_argument("Invalid owner task text layout input");
    }

    std::string normalized(text);
    std::replace(normalized.begin(), normalized.end(), '\t', ' ');
    std::string result;
    result.reserve(normalized.size());
    std::size_t start = 0;
    for (;;) {
        const auto newline = normalized.find('\n', start);
        const auto length =
            newline == std::string::npos ? normalized.size() - start : newline - start;
        WrapParagraph(std::string_view(normalized).substr(start, length), width, font_size, shape,
                      result);
        if (newline == std::string::npos) {
            break;
        }
        result.push_back('\n');
        start = newline + 1;
    }
    return result;
}
} // namespace prism::runtime
