// Rails: app/models/sound.rb, app/models/message.rb (`sound`). Rust: crates/db/src/models/sound.rs.
#include "models/sound.hpp"

#include <algorithm>
#include <array>

namespace campfire::models::sounds {

namespace {

struct Row {
  const char* name;
  const char* text;        // nullptr for a sound with an image
  const char* image_file;  // nullptr for a sound with text
  std::uint32_t width;
  std::uint32_t height;
};

constexpr Row kRows[] = {
    {"56k", nullptr, "56k.webp", 79, 33},
    {"bell", "🔔", nullptr, 0, 0},
    {"bezos", "😆💭", nullptr, 0, 0},
    {"bueller", "anyone?", nullptr, 0, 0},
    {"butts", "👐 🚬", nullptr, 0, 0},
    {"clowntown", nullptr, "clowntown.webp", 210, 150},
    {"cottoneyejoe", "🎶🙉🎶 ", nullptr, 0, 0},
    {"crickets", "hears crickets chirping", nullptr, 0, 0},
    {"curb", nullptr, "curb.webp", 150, 101},
    {"dadgummit", "dad gummit!! 🎣", nullptr, 0, 0},
    {"dangerzone", nullptr, "dangerzone.webp", 157, 32},
    {"danielsan", "🎆 🏆 🎆", nullptr, 0, 0},
    {"deeper", nullptr, "top.webp", 188, 80},
    {"ballmer", "developers!", nullptr, 0, 0},
    {"donotwant", nullptr, "donotwant.webp", 150, 150},
    {"drama", nullptr, "drama.webp", 300, 16},
    {"flawless", "#flawless", nullptr, 0, 0},
    {"glados", "🤖💢", nullptr, 0, 0},
    {"gogogo", "Go, go, go!", nullptr, 0, 0},
    {"greatjob", nullptr, "greatjob.webp", 79, 16},
    {"greyjoy", "😖🎺", nullptr, 0, 0},
    {"guarantee", "guarantees it 👌", nullptr, 0, 0},
    {"heygirl", "✨💁✨", nullptr, 0, 0},
    {"honk", "HONK", nullptr, 0, 0},
    {"horn", "🐶 ✂️ 🐱", nullptr, 0, 0},
    {"horror", "💀 💀 💀 💀 💀 💀 💀", nullptr, 0, 0},
    {"inconceivable", "doesn't think it means what you think it means…", nullptr, 0, 0},
    {"letitgo", "❄️👩❄️⛄️❄️", nullptr, 0, 0},
    {"live", "is DOING IT LIVE", nullptr, 0, 0},
    {"loggins", nullptr, "loggins.webp", 200, 151},
    {"makeitso", "make it so 👉", nullptr, 0, 0},
    {"noooo", "👸💀😒", nullptr, 0, 0},
    {"nyan", nullptr, "nyan.webp", 36, 15},
    {"ohmy", "raises an eyebrow 😏", nullptr, 0, 0},
    {"ohyeah", "isn't playing by the rules", nullptr, 0, 0},
    {"pushit", nullptr, "pushit.webp", 104, 15},
    {"rimshot", "plays a rimshot", nullptr, 0, 0},
    {"rollout", "is rolling out 🚗", nullptr, 0, 0},
    {"rumble", nullptr, "rumble.webp", 220, 150},
    {"sax", "🌇🎷🎶", nullptr, 0, 0},
    {"secret", "found a secret area 🔑", nullptr, 0, 0},
    {"sexyback", "🔞", nullptr, 0, 0},
    {"story", "and now you know…", nullptr, 0, 0},
    {"tada", "plays a fanfare 🎏", nullptr, 0, 0},
    {"tmyk", "✨ ⭐️ The More You Know ✨ ⭐️", nullptr, 0, 0},
    {"totes", "😁👍", nullptr, 0, 0},
    {"trololo", "трололо", nullptr, 0, 0},
    {"trombone", "plays a sad trombone", nullptr, 0, 0},
    {"unix", "knows this 💻", nullptr, 0, 0},
    {"vuvuzela", "======<() ~ ♪ ~♫", nullptr, 0, 0},
    {"what", nullptr, "what.webp", 100, 131},
    {"whoomp", "👏‼️😎", nullptr, 0, 0},
    {"wups", "wups!", nullptr, 0, 0},
    {"yay", nullptr, "yay.webp", 103, 50},
    {"yeah", nullptr, "yeah.webp", 104, 15},
    {"yodel", "📣🗻🙉", nullptr, 0, 0},
};

}  // namespace

const Sound* find_by_name(std::string_view name) {
  static const auto table = [] {
    std::array<Sound, std::size(kRows)> sounds;
    for (std::size_t i = 0; i < std::size(kRows); ++i) {
      const Row& row = kRows[i];
      Sound& sound = sounds[i];
      sound.name = row.name;
      if (row.image_file != nullptr) {
        sound.image = SoundImage{row.image_file, row.width, row.height};
      } else {
        sound.text = std::string_view(row.text);
      }
    }
    return sounds;
  }();
  const auto it = std::ranges::find_if(table, [&](const Sound& s) { return s.name == name; });
  return it == table.end() ? nullptr : &*it;
}

const Sound* sound_in(std::string_view plain_text) {
  constexpr std::string_view kPrefix = "/play ";
  if (!plain_text.starts_with(kPrefix)) return nullptr;
  const std::string_view name = plain_text.substr(kPrefix.size());
  if (name.empty()) return nullptr;
  const bool word = std::ranges::all_of(name, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  });
  return word ? find_by_name(name) : nullptr;
}

}  // namespace campfire::models::sounds
