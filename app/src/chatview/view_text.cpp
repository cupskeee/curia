// SPDX-License-Identifier: MIT
#include "chatview/view_text.h"

#include "advisor/snapshot_renderer.h"

namespace curia::chatview::text {

std::string seenFrom(const bridge::Date& date) {
    return std::string(kSeenFromStart) + advisor::formatGameDate(date) + std::string(kSeenFromEnd);
}

}  // namespace curia::chatview::text
