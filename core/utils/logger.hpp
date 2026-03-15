#pragma once
#include <string>

// Function prototyping for log and error messages

namespace gasoline {

void log(const std::string& message); 
void error(const std::string& message);

}