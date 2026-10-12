// SPDX-License-Identifier: MIT
#include "core/ui_message.h"

#include <utility>

namespace curia {

bool mergeUiMessages(UiMessage& pending, UiMessage& incoming) {
    auto* first = std::get_if<ProviderToken>(&pending);
    auto* second = std::get_if<ProviderToken>(&incoming);
    if (first == nullptr || second == nullptr) {
        return false;
    }
    if (first->generation != second->generation || first->kind != second->kind) {
        return false;
    }
    if (first->text.size() + second->text.size() > kMaxCoalescedTokenBytes) {
        return false;
    }
    first->text += second->text;
    return true;
}

std::shared_ptr<UiMailbox> makeUiMailbox(UiMailbox::Wake wake, std::size_t maxMessages) {
    UiMailbox::Options options;
    options.maxMessages = maxMessages;
    options.wake = std::move(wake);
    options.merge = mergeUiMessages;
    return std::make_shared<UiMailbox>(std::move(options));
}

}  // namespace curia
