#include <core/context.hpp>

namespace soundstep {

void context::notify(std::string text, widget_message_kind kind, std::chrono::milliseconds duration)
{
    message.text = std::move(text);
    message.kind = kind;
    message.expires_at = std::chrono::steady_clock::now() + duration;
}

const widget_message* context::active_message() noexcept
{
    if (message.text.empty() || std::chrono::steady_clock::now() >= message.expires_at) {
        message.text.clear();
        return nullptr;
    }
    return &message;
}

}