#pragma once

#include <stdexcept>
#include <string>

#define custom_exception(name, base)                                                                                   \
  class name : public base {                                                                                           \
  public:                                                                                                              \
    name() : base(#name) {}                                                                                            \
    name(const char *message) : base(message) {}                                                                       \
    name(const std::string &message) : base(message) {}                                                                \
  };

custom_exception(GeoBox_Error, std::runtime_error);
custom_exception(Overflow_Check_Error, GeoBox_Error);
