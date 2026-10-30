#pragma once

#include <string>

namespace beacon {

struct UpdateNotice {
    std::string version;
    std::string error;
    bool apply_requested = false;
};

}  // namespace beacon
