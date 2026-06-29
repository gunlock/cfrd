#pragma once
#include <optional>
#include <string>

struct CFRChunk {
  std::string date;
  std::string title;
  std::optional<std::string> chapter;
  std::optional<std::string> subchapter;
  std::optional<std::string> part;
  std::optional<std::string> subpart;
  std::optional<std::string> section; // formatted as "part.section", e.g. "61.1"
  std::optional<std::string> appendix;
};
