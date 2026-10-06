// Build-time generator: Propshaft precompile, import map and compressed bodies
// (Rails: Propshaft assets:precompile; Rust: crates/assets/build.rs).
// Usage: campfire_assets_gen <rails-assets-dir> <out-dir>. It writes assets_blob.bin and assets_data.cpp.
#include <libdeflate.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "assets/importmap.hpp"
#include "assets/mime.hpp"
#include "assets/propshaft.hpp"
#include "assets/table.hpp"
#include "compat/ruby.hpp"

namespace fs = std::filesystem;
using namespace campfire;          // NOLINT(google-build-using-namespace)
using namespace campfire::assets;  // NOLINT(google-build-using-namespace)

namespace {

// Propshaft's default, which the app does not change (config.assets.prefix).
constexpr std::string_view kPrefix = "/assets";

[[noreturn]] void die(const std::string& message) {
  std::cerr << "campfire_assets_gen: " << message << '\n';
  std::exit(1);
}

template <class T>
T must(Result<T> result) {
  if (!result) {
    die(result.error().message);
  }
  return std::move(*result);
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    die("cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// JSON.generate string escape: no script_safe, and non-ASCII bytes pass through.
std::string json(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (b < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", b);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

// A C++ string literal with octal escapes for each byte that is not printable ASCII.
std::string cpp_literal(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (b >= 0x20 && b < 0x7f && c != '?') {
      out += c;
    } else {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\%03o", b);
      out += buf;
    }
  }
  out += '"';
  return out;
}

void all_files(const fs::path& dir, std::vector<fs::path>& files) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_directory()) {
      all_files(entry.path(), files);
    } else {
      files.push_back(entry.path());
    }
  }
}

// config/initializers/assets.rb sets `Rails.application.config.assets.version`.
std::string assets_version(const fs::path& rails_root) {
  const fs::path file = rails_root / "config/initializers/assets.rb";
  if (!fs::exists(file)) {
    return "1";
  }
  std::istringstream lines(read_text(file));
  for (std::string line; std::getline(lines, line);) {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const auto key = line.find("config.assets.version");
    if (key == std::string::npos) {
      continue;
    }
    auto value = line.substr(key + 21);
    const auto eq = value.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    value = value.substr(eq + 1);
    const auto begin = value.find_first_not_of(" \t");
    const auto end = value.find_last_not_of(" \t\r");
    value = begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
    while (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
      value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
      value.pop_back();
    }
    return value;
  }
  return "1";
}

// `overrides/` first, then the lines of LOAD_PATH (written by script/revendor of the Rust repo).
std::vector<fs::path> load_path_dirs(const fs::path& root) {
  std::vector<fs::path> dirs{root / "overrides"};
  std::istringstream lines(read_text(root / "LOAD_PATH"));
  for (std::string line; std::getline(lines, line);) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }
    const auto colon = line.find(':');
    const std::string kind = line.substr(0, colon);
    const std::string dir = line.substr(colon + 1);
    if (kind == "reference") {
      dirs.push_back(root / "reference" / dir);
    } else if (kind == "vendor") {
      dirs.push_back(root / "vendor" / dir);
    } else {
      die("LOAD_PATH: bad line " + line);
    }
  }
  return dirs;
}

// The bytes of the blob, and the ranges in it.
class Blob {
 public:
  BlobRef add(std::string_view bytes) {
    BlobRef ref{static_cast<std::uint32_t>(data_.size()), static_cast<std::uint32_t>(bytes.size())};
    data_.append(bytes);
    return ref;
  }
  [[nodiscard]] const std::string& data() const { return data_; }

 private:
  std::string data_;
};

std::string gzip(std::string_view in) {
  libdeflate_compressor* c = libdeflate_alloc_compressor(12);
  std::string out(libdeflate_gzip_compress_bound(c, in.size()), '\0');
  const std::size_t n = libdeflate_gzip_compress(c, in.data(), in.size(), out.data(), out.size());
  libdeflate_free_compressor(c);
  if (n == 0) {
    die("gzip failed");
  }
  out.resize(n);
  return out;
}

std::string zstd(std::string_view in) {
  std::string out(ZSTD_compressBound(in.size()), '\0');
  const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 19);
  if (ZSTD_isError(n) != 0U) {
    die("zstd failed");
  }
  out.resize(n);
  return out;
}

// The types that the front server compresses, with JSON and XML as well.
bool worth_compressing(std::string_view content_type) {
  return compressible(content_type) || content_type.starts_with("application/json") ||
         content_type.starts_with("application/xml");
}

struct File {
  std::string url;
  std::string content;  // identity body
};

FileRecord make_record(Blob& blob, const File& file) {
  FileRecord record{};
  record.identity = blob.add(file.content);
  const auto type = mime_type(file_extname(file.url));
  if (type && worth_compressing(*type) && !file.content.empty()) {
    const std::string gz = gzip(file.content);
    if (gz.size() < file.content.size()) {
      record.gzip = blob.add(gz);
    }
    const std::string zs = zstd(file.content);
    if (zs.size() < file.content.size()) {
      record.zstd = blob.add(zs);
    }
  }
  return record;
}

// Importmap::ImportmapTagsHelper#javascript_importmap_tags for "application", with no CSP nonce.
std::string importmap_tags(const build::LoadPath& lp, const fs::path& rails_root) {
  const auto resolve = [&](const std::string& path) -> std::optional<std::string> {
    const auto index = lp.find(path);
    if (!index) {
      return std::nullopt;
    }
    return std::string(kPrefix) + "/" + lp.digested_path(*index);
  };
  const auto pins = must(build::expand_importmap(rails_root / "config/importmap.rb", rails_root));
  // A missing asset is skipped, as a rescued Propshaft::MissingAssetError is.
  std::vector<std::pair<std::string, std::string>> imports;
  for (const auto& pin : pins) {
    if (auto path = resolve(pin.path)) {
      imports.emplace_back(pin.name, std::move(*path));
    }
  }
  std::string map;
  if (imports.empty()) {
    map = "{\n  \"imports\": {}\n}";
  } else {
    map = "{\n  \"imports\": {\n";
    for (std::size_t i = 0; i < imports.size(); ++i) {
      map += "    " + json(imports[i].first) + ": " + json(imports[i].second);
      map += i + 1 < imports.size() ? ",\n" : "\n";
    }
    map += "  }\n}";
  }
  std::vector<std::string> preloads;
  for (const auto& pin : pins) {
    if (!pin.preload) {
      continue;
    }
    auto path = resolve(pin.path);
    if (path && std::find(preloads.begin(), preloads.end(), *path) == preloads.end()) {
      preloads.push_back(std::move(*path));
    }
  }
  std::string tags = "<script type=\"importmap\" data-turbo-track=\"reload\">" + map + "</script>\n";
  for (std::size_t i = 0; i < preloads.size(); ++i) {
    tags += "<link rel=\"modulepreload\" href=\"" + compat::html_escape(preloads[i]) + "\">";
    tags += i + 1 < preloads.size() ? "\n" : "";
  }
  tags += "\n<script type=\"module\">import \"application\"</script>";
  return tags;
}

void write_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    die("cannot write " + path.string());
  }
}

std::string blob_ref(const BlobRef& r) {
  return "{" + std::to_string(r.offset) + "u, " + std::to_string(r.size) + "u}";
}

std::int64_t build_time() {
  if (const char* epoch = std::getenv("SOURCE_DATE_EPOCH")) {  // NOLINT(concurrency-mt-unsafe)
    return std::atoll(epoch);
  }
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    die("usage: campfire_assets_gen <rails-assets-dir> <out-dir>");
  }
  const fs::path root = fs::absolute(argv[1]);
  const fs::path out_dir = fs::absolute(argv[2]);
  const fs::path rails_root = root / "reference";
  fs::create_directories(out_dir);

  auto lp = must(build::LoadPath::create(load_path_dirs(root), assets_version(rails_root), std::string(kPrefix)));

  // Files that ActionDispatch::Static can serve: reference/public, then the precompiled assets.
  std::vector<File> files;
  const fs::path pub = rails_root / "public";
  std::vector<fs::path> public_files;
  all_files(pub, public_files);
  for (const auto& file : public_files) {
    const fs::path relative = file.lexically_relative(pub);
    if (*relative.begin() == "assets") {
      continue;  // A stray precompile must not hide what this tool builds.
    }
    files.push_back(File{"/" + relative.string(), read_text(file)});
  }

  std::vector<std::pair<std::string, std::string>> manifest;  // logical, digested
  std::string manifest_json = "{";
  std::vector<std::string> stylesheets;
  for (std::size_t i = 0; i < lp.assets().size(); ++i) {
    const std::string& logical = lp.assets()[i].logical_path;
    std::string digested = lp.digested_path(i);
    auto compiled = must(lp.compiled_content(i));
    std::string body = compiled ? *compiled : lp.assets()[i].content;
    files.push_back(File{std::string(kPrefix) + "/" + digested, std::move(body)});
    // Propshaft::Processor#write_manifest, in the order of the load path.
    manifest_json +=
        (i == 0 ? "" : ",") + json(logical) + ":{\"digested_path\":" + json(digested) + ",\"integrity\":null}";
    if (build::extname(logical) == ".css") {
      stylesheets.push_back(logical);
    }
    manifest.emplace_back(logical, std::move(digested));
  }
  manifest_json += "}";
  files.push_back(File{std::string(kPrefix) + "/.manifest.json", manifest_json});

  std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.url < b.url; });
  files.erase(std::unique(files.begin(), files.end(), [](const File& a, const File& b) { return a.url == b.url; }),
              files.end());
  std::sort(manifest.begin(), manifest.end());
  std::sort(stylesheets.begin(), stylesheets.end());

  Blob blob;
  std::string cpp = "// Generated by campfire_assets_gen. Do not edit.\n#include \"assets/table.hpp\"\n\n";
  cpp += "extern \"C\" const char campfire_assets_blob[];\n";
  cpp +=
      "__asm__(\".section .rodata\\n.balign 16\\n.global campfire_assets_blob\\ncampfire_assets_blob:\\n"
      ".incbin \\\"" +
      (out_dir / "assets_blob.bin").string() + "\\\"\\n.byte 0\\n.previous\\n\");\n\n";
  cpp += "namespace campfire::assets {\nnamespace {\n\n";
  cpp += "constexpr FileRecord kFiles[] = {\n";
  for (const auto& file : files) {
    const FileRecord r = make_record(blob, file);
    cpp += "    {" + cpp_literal(file.url) + ", " + blob_ref(r.identity) + ", " + blob_ref(r.gzip) + ", " +
           blob_ref(r.zstd) + "},\n";
  }
  cpp += "};\n\nconstexpr ManifestRecord kManifest[] = {\n";
  for (const auto& [logical, digested] : manifest) {
    cpp += "    {" + cpp_literal(logical) + ", " + cpp_literal(digested) + "},\n";
  }
  cpp += "};\n\nconstexpr std::string_view kStylesheets[] = {\n";
  for (const auto& s : stylesheets) {
    cpp += "    " + cpp_literal(s) + ",\n";
  }
  const BlobRef manifest_ref = blob.add(manifest_json);
  const BlobRef importmap_ref = blob.add(importmap_tags(lp, rails_root));
  cpp += "};\n\n}  // namespace\n\n";
  cpp +=
      "const GeneratedData& generated_data() noexcept {\n  static const GeneratedData data{campfire_assets_blob, "
      "kFiles, kManifest, "
      "kStylesheets,\n                                  " +
      blob_ref(manifest_ref) + ", " + blob_ref(importmap_ref) + ", " + std::to_string(build_time()) +
      "};\n  return data;\n}\n\n}  // namespace campfire::assets\n";

  write_file(out_dir / "assets_blob.bin", blob.data());
  write_file(out_dir / "assets_data.cpp", cpp);
  std::cout << "campfire_assets_gen: " << files.size() << " files, " << manifest.size() << " assets, "
            << blob.data().size() << " bytes\n";
  return 0;
}
