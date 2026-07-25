#pragma once

#include <cstdint>
#include <vector>

#include "Reading.hpp"

struct Snapshot {
    int64_t              timestamp;
    std::vector<Reading> readings;
};
