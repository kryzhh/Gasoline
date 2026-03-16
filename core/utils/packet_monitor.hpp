#pragma once

#include <string>
namespace gasoline {
void log_rx(const std::string& device_id, const std::string& type);
void log_tx(const std::string& device_id, const std::string& type);
}