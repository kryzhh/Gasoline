#include "device_id.hpp"

#include <unistd.h>
#include <functional>

namespace gasoline {

#include <unistd.h>

std::string get_my_device_id() {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    std::string base = std::string(hostname) + "_" + std::to_string(getpid());

    size_t hash = std::hash<std::string>{}(base);
    return std::to_string(hash);

}

}