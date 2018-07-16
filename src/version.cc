#include "version.h"

std::uint32_t GetVersionInt(int major, int minor, int patch) {
  return major * 1000000 + minor * 1000 + patch;
}

std::string GetVersionStr(int major, int minor, int patch, bool release) {
  auto v = std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch);
  if (release) return v;
  return v + "-dev";
}
