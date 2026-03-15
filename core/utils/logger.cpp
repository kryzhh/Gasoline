#include "logger.hpp"
#include <iostream>

// Function declaration for logger

namespace gasoline {

void log(const std::string& message) {
    std::cout << "[gasoline] " << message << std::endl;
}

void error(const std::string& message) {
    std::cerr << "[gasoline:error] " << message << std::endl;
}

}