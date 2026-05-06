#pragma once

#include <cstddef>
#include <string>

bool writeAll(int fd, const char* data, size_t len);
std::string urlDecode(const std::string& s);
