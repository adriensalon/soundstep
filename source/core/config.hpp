#pragma once

#include <filesystem>

namespace soundstep {

struct configuration {
    std::filesystem::path library_path;
    bool scan_subdirectories { true };
    bool scan_on_startup { true };
    bool lan_discovery_enabled { true };
};

}
