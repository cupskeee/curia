// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "probe_combos.h"

using namespace curia::probe;

TEST_CASE("the combination table covers every level with every flag set") {
    const auto& combos = allCombos();
    REQUIRE(combos.size() == 20);
    CHECK(combos.front().number == 1);
    CHECK(combos.back().number == 20);

    std::set<std::pair<int, char>> seen;
    for (std::size_t i = 0; i < combos.size(); ++i) {
        CHECK(combos[i].number == static_cast<int>(i) + 1);
        seen.insert({combos[i].level, combos[i].flagSet});
    }
    CHECK(seen.size() == combos.size());
}

TEST_CASE("combination 1 is the screen-saver level with the recipe flags, 2 adds "
          "canJoinAllApplications") {
    const Combo* first = comboByNumber(1);
    REQUIRE(first != nullptr);
    CHECK(first->level == 1000);
    CHECK(std::string(first->levelName) == "screenSaver");
    CHECK(first->flagSet == 'A');
    CHECK(flagNames(first->flags) == "canJoinAllSpaces|fullScreenAuxiliary|stationary");

    const Combo* second = comboByNumber(2);
    REQUIRE(second != nullptr);
    CHECK(second->level == 1000);
    CHECK((second->flags & kJoinAllApplications) != 0U);
}

TEST_CASE("the last four combinations use the floating level") {
    for (int number = 17; number <= 20; ++number) {
        const Combo* combo = comboByNumber(number);
        REQUIRE(combo != nullptr);
        CHECK(combo->level == 3);
    }
}

TEST_CASE("comboByNumber rejects numbers outside the table") {
    CHECK(comboByNumber(0) == nullptr);
    CHECK(comboByNumber(-1) == nullptr);
    CHECK(comboByNumber(21) == nullptr);
    CHECK(comboByNumber(20) != nullptr);
}

TEST_CASE("flagNames and describeCombo") {
    CHECK(flagNames(0) == "none");
    CHECK(flagNames(kStationary) == "stationary");
    CHECK(describeCombo(*comboByNumber(1)) == "combo 1/20 level 1000 (screenSaver) flags A "
                                              "canJoinAllSpaces|fullScreenAuxiliary|stationary");
}

TEST_CASE("comboIndexAt follows the step timer") {
    CHECK(comboIndexAt(0.0, 5.0, 20) == 0);
    CHECK(comboIndexAt(4.99, 5.0, 20) == 0);
    CHECK(comboIndexAt(5.0, 5.0, 20) == 1);
    CHECK(comboIndexAt(99.9, 5.0, 20) == 19);
    CHECK(comboIndexAt(100.0, 5.0, 20) == -1);
    CHECK(comboIndexAt(-0.1, 5.0, 20) == -1);
    CHECK(comboIndexAt(1.0, 0.0, 20) == -1);
    CHECK(comboIndexAt(1.0, 5.0, 0) == -1);
}
