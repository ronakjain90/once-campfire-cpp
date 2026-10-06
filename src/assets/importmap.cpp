// importmap-rails 2.2.2 Importmap::Map for config/importmap.rb (Rust: crates/assets/build/importmap.rs).
#include "assets/importmap.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <variant>

#include "assets/propshaft.hpp"

namespace campfire::assets::build {

namespace fs = std::filesystem;

namespace {

struct PinEntry {
  std::string name;
  std::optional<std::string> to;
  bool preload = true;
};

struct DirEntry {
  std::string dir;
  std::optional<std::string> under;
  std::optional<std::string> to;
  bool preload = true;
};

using Entry = std::variant<PinEntry, DirEntry>;

std::string_view trim(std::string_view s) {
  constexpr std::string_view kSpace = " \t\r\n\v\f";
  const auto first = s.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

std::string_view ltrim(std::string_view s) {
  const auto first = s.find_first_not_of(" \t\r\n\v\f");
  return first == std::string_view::npos ? std::string_view{} : s.substr(first);
}

// A reader for the arguments after the command name.
struct Args {
  std::string_view rest;

  std::optional<std::string> string() {
    if (rest.empty() || (rest.front() != '"' && rest.front() != '\'')) {
      return std::nullopt;
    }
    const auto end = rest.find(rest.front(), 1);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    std::string value(rest.substr(1, end - 1));
    rest = ltrim(rest.substr(end + 1));
    return value;
  }

  bool keyword(std::string_view word) {
    if (!rest.starts_with(word)) {
      return false;
    }
    rest = ltrim(rest.substr(word.size()));
    return true;
  }
};

using Value = std::variant<std::string, bool>;

Result<std::optional<std::pair<std::string, Value>>> next_option(Args& args, std::string_view line) {
  if (args.rest.empty() || args.rest.front() == '#') {
    return std::optional<std::pair<std::string, Value>>{};
  }
  if (!args.rest.starts_with(',')) {
    return fail(Errc::Parse, "importmap.rb: cannot parse " + std::string(args.rest));
  }
  const std::string_view after = ltrim(args.rest.substr(1));
  const auto colon = after.find(':');
  if (colon == std::string_view::npos) {
    return fail(Errc::Parse, "importmap.rb: expected key: value in " + std::string(line));
  }
  std::string key(trim(after.substr(0, colon)));
  args.rest = ltrim(after.substr(colon + 1));
  Value value;
  if (auto s = args.string()) {
    value = std::move(*s);
  } else if (args.keyword("true")) {
    value = true;
  } else if (args.keyword("false")) {
    value = false;
  } else {
    return fail(Errc::Parse, "importmap.rb: unsupported value " + std::string(args.rest));
  }
  return std::optional<std::pair<std::string, Value>>(std::pair{std::move(key), std::move(value)});
}

Result<std::optional<Entry>> parse_line(std::string_view raw) {
  const std::string_view line = trim(raw);
  if (line.empty() || line.front() == '#') {
    return std::optional<Entry>{};
  }
  const auto space = line.find_first_of(" \t");
  const std::string_view command = line.substr(0, space);
  Args args{space == std::string_view::npos ? std::string_view{} : ltrim(line.substr(space))};
  auto first = args.string();
  if (!first) {
    return fail(Errc::Parse, "importmap.rb: expected a string in " + std::string(line));
  }
  std::optional<std::string> to;
  std::optional<std::string> under;
  bool preload = true;
  for (;;) {
    auto option = next_option(args, line);
    if (!option) {
      return std::unexpected(option.error());
    }
    if (!*option) {
      break;
    }
    auto& [key, value] = **option;
    if (key == "to" && std::holds_alternative<std::string>(value)) {
      to = std::get<std::string>(value);
    } else if (key == "under" && std::holds_alternative<std::string>(value)) {
      under = std::get<std::string>(value);
    } else if (key == "preload" && std::holds_alternative<bool>(value)) {
      preload = std::get<bool>(value);
    } else {
      return fail(Errc::Parse, "importmap.rb: unsupported option " + key + " in " + std::string(line));
    }
  }
  if (command == "pin") {
    return std::optional<Entry>(PinEntry{std::move(*first), std::move(to), preload});
  }
  if (command == "pin_all_from") {
    return std::optional<Entry>(DirEntry{std::move(*first), std::move(under), std::move(to), preload});
  }
  return fail(Errc::Parse, "importmap.rb: unsupported statement " + std::string(command));
}

void insert(std::vector<Pin>& packages, Pin pin) {
  const auto it = std::find_if(packages.begin(), packages.end(), [&](const Pin& p) { return p.name == pin.name; });
  if (it == packages.end()) {
    packages.push_back(std::move(pin));
  } else {
    *it = std::move(pin);
  }
}

// `[under, filename.chomp(extname).remove(/(?:\/|^)index$/).presence].compact.join("/")`
std::string module_name_from(std::string_view filename, const std::optional<std::string>& under) {
  const std::string_view ext = extname(filename);
  std::string_view stem = filename.substr(0, filename.size() - ext.size());
  if (stem == "index") {
    stem = {};
  } else if (stem.ends_with("/index")) {
    stem.remove_suffix(6);
  }
  std::string out = under.value_or("");
  if (!stem.empty()) {
    if (under) {
      out += '/';
    }
    out += stem;
  }
  return out;
}

// `Dir[path.join("**/*.js{,m}")]`: a Dir glob skips dotfiles and dot-directories.
void javascript_files_in_tree(const fs::path& dir, std::vector<fs::path>& files) {
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with('.')) {
      continue;
    }
    if (entry.is_directory()) {
      javascript_files_in_tree(entry.path(), files);
    } else if (name.ends_with(".js") || name.ends_with(".jsm")) {
      files.push_back(entry.path());
    }
  }
}

}  // namespace

Result<std::vector<Pin>> expand_importmap(const fs::path& importmap_rb, const fs::path& rails_root) {
  std::ifstream in(importmap_rb, std::ios::binary);
  if (!in) {
    return fail(Errc::Io, "cannot read " + importmap_rb.string());
  }
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  std::vector<Pin> packages;
  std::vector<DirEntry> directories;
  std::size_t pos = 0;
  while (pos <= source.size()) {
    const std::size_t end = std::min(source.find('\n', pos), source.size());
    auto parsed = parse_line(std::string_view(source).substr(pos, end - pos));
    pos = end + 1;
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    if (!*parsed) {
      continue;
    }
    if (auto* pin = std::get_if<PinEntry>(&**parsed)) {
      std::string path = pin->to.value_or(pin->name + ".js");
      insert(packages, Pin{pin->name, std::move(path), pin->preload});
    } else {
      auto& dir = std::get<DirEntry>(**parsed);
      std::erase_if(directories, [&](const DirEntry& d) { return d.dir == dir.dir; });
      directories.push_back(std::move(dir));
    }
  }

  for (const auto& dir : directories) {
    const fs::path root = rails_root / dir.dir;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
      continue;
    }
    std::vector<fs::path> files;
    javascript_files_in_tree(root, files);
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      const std::string filename = file.lexically_relative(root).string();
      std::string path;
      const std::optional<std::string>& prefix = dir.to ? dir.to : dir.under;
      if (prefix && !prefix->empty()) {
        path = *prefix + "/";
      }
      path += filename;
      insert(packages, Pin{module_name_from(filename, dir.under), std::move(path), dir.preload});
    }
  }
  return packages;
}

}  // namespace campfire::assets::build
