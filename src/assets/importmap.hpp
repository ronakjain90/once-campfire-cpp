// importmap-rails 2.2.2 Importmap::Map for config/importmap.rb (Rust: crates/assets/build/importmap.rs).
// Build-time code. It reads only `pin` and `pin_all_from` with `to:`, `under:` and `preload:`.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace campfire::assets::build {

struct Pin {
  std::string name;
  std::string path;
  bool preload = true;
};

// Importmap::Map#expanded_packages_and_directories: pins in order of insertion, then each
// directory expanded. A later entry for a known name keeps the position of the first.
[[nodiscard]] Result<std::vector<Pin>> expand_importmap(const std::filesystem::path& importmap_rb,
                                                        const std::filesystem::path& rails_root);

}  // namespace campfire::assets::build
