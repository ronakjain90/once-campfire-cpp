// Propshaft 1.2.1: load path, digests, CSS and JS compilers (Rust: crates/assets/build/propshaft.rs).
// Build-time code. The runtime library does not link it.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/error.hpp"

namespace campfire::assets::build {

struct SourceAsset {
  std::string logical_path;
  std::filesystem::path source;
  std::string content;
};

class Regex;

enum class Kind { Css, Js, Other };

class LoadPath {
 public:
  // Propshaft::LoadPath#assets_by_path: an earlier directory wins for the same logical path.
  // Dotfiles are skipped (dot-directories are not).
  [[nodiscard]] static Result<LoadPath> create(const std::vector<std::filesystem::path>& dirs, std::string version,
                                               std::string prefix);

  [[nodiscard]] const std::vector<SourceAsset>& assets() const noexcept { return assets_; }
  [[nodiscard]] std::optional<std::size_t> find(std::string_view logical_path) const;
  [[nodiscard]] Kind kind(std::size_t index) const;
  // Propshaft::Asset#digest: 8 hex digits of SHA-1.
  [[nodiscard]] const std::string& digest(std::size_t index) const;
  // Propshaft::Asset#digested_path
  [[nodiscard]] std::string digested_path(std::size_t index) const;
  // Propshaft::Compilers#compile: nullopt when the asset type has no compiler.
  [[nodiscard]] Result<std::optional<std::string>> compiled_content(std::size_t index) const;
  [[nodiscard]] const std::string& prefix() const noexcept { return prefix_; }

 private:
  LoadPath() = default;
  void collect_references(std::size_t index, const Regex& pattern, std::vector<std::size_t>& out) const;

  std::vector<SourceAsset> assets_;
  std::unordered_map<std::string, std::size_t> by_logical_;
  std::string version_;
  std::string prefix_;
  mutable std::vector<std::string> digests_;
};

// File.extname
[[nodiscard]] std::string_view extname(std::string_view path) noexcept;
[[nodiscard]] std::string hex(std::string_view bytes);

}  // namespace campfire::assets::build
