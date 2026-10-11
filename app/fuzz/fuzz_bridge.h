// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

// The fuzz input format of fuzz_bridge.cpp (the fixture format of tests/fixtures/curia1/README.md)
// run through the bridge in one piece; the events as the JSON text of the golden files. Lets the
// replay driver compare a fixture with its golden file.
std::string fuzzEventsJson(std::string_view input);
