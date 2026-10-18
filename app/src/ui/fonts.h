// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

// The bundled font (FR-OVL-3): Gentium Book Plus 6.200 by SIL International, Regular and Bold,
// unmodified, under the SIL Open Font License 1.1 (app/resources/fonts/README.md). The bytes are
// part of the executable: cmake/EmbedFile.cmake writes the definitions below at build time from
// the TTF files, so nothing is read from disk or the network at run time.
namespace curia::ui {

struct EmbeddedBytes {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};

EmbeddedBytes bundledRegularFont();
EmbeddedBytes bundledBoldFont();

}  // namespace curia::ui
