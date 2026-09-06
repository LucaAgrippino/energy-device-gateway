#pragma once

#include <cstdint>
#include <vector>

#include "Reading.hpp"

// Move-only by construction rather than by declaration: `readings` holds
// move-only `Reading`s, which implicitly deletes Snapshot's copy operations.
// DESIGN.md §3 spells the special members out; they would be redundant here.
struct Snapshot {
    int64_t              timestamp{0};
    std::vector<Reading> readings;
};
