// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>

#include "core/ui_message.h"

namespace curia::ui {

// The mailbox's wake-up in the app: pushes one SDL event of a type registered for it. The mailbox
// calls it on the producer's thread (the log watcher, the provider's net thread) when it goes from
// empty to non-empty; SDL_PushEvent may be called from any thread. After disarm() nothing more is
// pushed (a producer can outlive the event loop by a moment while the app shuts down).
class SdlWake {
public:
    explicit SdlWake(Uint32 eventType) : type_(eventType) {}

    UiMailbox::Wake function() const {
        const Uint32 type = type_;
        const std::shared_ptr<std::atomic<bool>> armed = armed_;
        return [type, armed] {
            if (!armed->load()) {
                return;
            }
            SDL_Event event;
            SDL_zero(event);
            event.type = type;
            SDL_PushEvent(&event);
        };
    }

    void disarm() { armed_->store(false); }
    Uint32 eventType() const { return type_; }

private:
    Uint32 type_;
    std::shared_ptr<std::atomic<bool>> armed_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace curia::ui
