// Rails: app/models/sound.rb. Rust: crates/db/src/models/sound.rs.
// The sounds of `/play <name>`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::models {

struct SoundImage {
  std::string_view file;  // `sounds/<file>` is the asset path
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct Sound {
  std::string_view name;
  std::optional<std::string_view> text;
  std::optional<SoundImage> image;
};

namespace sounds {

// `Sound.find_by_name`
[[nodiscard]] const Sound* find_by_name(std::string_view name);
// `plain_text_body.match(/\A\/play (?<name>\w+)\z/)`, then `Sound.find_by_name`.
[[nodiscard]] const Sound* sound_in(std::string_view plain_text);

}  // namespace sounds
}  // namespace campfire::models
