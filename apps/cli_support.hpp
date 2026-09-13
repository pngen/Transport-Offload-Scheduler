// Small argument parser shared by the command line tools.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_APPS_CLI_SUPPORT_HPP
#define TOS_APPS_CLI_SUPPORT_HPP

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace tos_cli {

struct Arguments {
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> options;

  [[nodiscard]] bool has(std::string_view name) const {
    for (const auto& option : options) {
      if (option.first == name) return true;
    }
    return false;
  }
  [[nodiscard]] std::string get(std::string_view name, std::string fallback = {}) const {
    for (const auto& option : options) {
      if (option.first == name) return option.second;
    }
    return fallback;
  }
  [[nodiscard]] std::uint64_t get_u64(std::string_view name, std::uint64_t fallback = 0) const {
    const std::string text = get(name);
    if (text.empty()) return fallback;
    return std::strtoull(text.c_str(), nullptr, 0);
  }
  [[nodiscard]] std::uint16_t get_u16(std::string_view name, std::uint16_t fallback = 0) const {
    return static_cast<std::uint16_t>(get_u64(name, fallback));
  }
  [[nodiscard]] int get_int(std::string_view name, int fallback = 0) const {
    const std::string text = get(name);
    if (text.empty()) return fallback;
    return static_cast<int>(std::strtol(text.c_str(), nullptr, 10));
  }
};

[[nodiscard]] inline Arguments parse(int argc, char** argv, int start_index = 1) {
  Arguments arguments;
  for (int i = start_index; i < argc; ++i) {
    const std::string token = argv[i];
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        arguments.options.emplace_back(token.substr(2, equals - 2), token.substr(equals + 1));
        continue;
      }
      const std::string name = token.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        arguments.options.emplace_back(name, argv[i + 1]);
        ++i;
      } else {
        arguments.options.emplace_back(name, std::string());
      }
      continue;
    }
    arguments.positional.push_back(token);
  }
  return arguments;
}

inline void print_usage(std::string_view program, std::string_view body) {
  std::cout << program << " - Transport Offload Scheduler\n\n" << body << std::endl;
}

}  // namespace tos_cli

#endif  // TOS_APPS_CLI_SUPPORT_HPP
