// SPDX-License-Identifier: MIT
// Windows and Linux: no global hot key in M4 (ui/hotkey.h); the in-game Advisor button opens the
// window there.
#include <cstdio>
#include <memory>

#include "ui/hotkey.h"

namespace curia::ui {

std::unique_ptr<HotKey> HotKey::install(Uint32 /*eventType*/, bool /*grab*/,
                                        std::string_view& status) {
    status = "unsupported";
    return nullptr;
}

HotKey::~HotKey() = default;

bool HotKey::sendTestPress() {
    return false;
}

int runHotKeySelfTest() {
    std::printf("CURIA_SELFTEST_FAIL hotkey unsupported\n");
    return 1;
}

}  // namespace curia::ui
