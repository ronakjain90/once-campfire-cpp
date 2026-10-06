// Propshaft 1.2.1: load path, digests, CSS and JS compilers (Rust: crates/assets/build/propshaft.rs).
#include "assets/propshaft.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "assets/regex.hpp"

namespace campfire::assets::build {

namespace fs = std::filesystem;

namespace {

// Ruby's \s, spelled out.
constexpr std::string_view kWs = R"re([ \t\n\x0B\x0C\r])re";

std::string quoted_url(std::string_view head, std::string_view excluded) {
  std::string p(head);
  p += R"re(\()re";
  p += kWs;
  p += R"re(*["']?(?!(?:)re";
  p += excluded;
  p += R"re())([^"' \t\n\x0B\x0C\r?#)]+)([#?][^"')]+)?)re";
  p += kWs;
  p += R"re(*["']?\))re";
  return p;
}

struct Patterns {
  Regex css;
  Regex js;
  Regex source_mapping;
  Regex already_digested;
};

const Patterns& patterns() {
  static const Patterns value = [] {
    auto must = [](Result<Regex> r) {
      if (!r) {
        std::fputs(r.error().message.c_str(), stderr);
        std::abort();
      }
      return std::move(*r);
    };
    std::string source_mapping = R"re((//|/\*)# sourceMappingURL=(.+\.map)()re";
    source_mapping += kWs;
    source_mapping += R"re(*?\*/)?)re";
    source_mapping += kWs;
    source_mapping += R"re(*?(?=\n?\z))re";
    return Patterns{must(Regex::compile(quoted_url("url", R"re(\#|%23|data:|http:|https:|//)re"))),
                    must(Regex::compile(quoted_url("RAILS_ASSET_URL", R"re(\#|%23|data|http|//)re"))),
                    must(Regex::compile(source_mapping)),
                    must(Regex::compile(R"re(-([0-9a-zA-Z_-]{7,128})\.digested)re"))};
  }();
  return value;
}

std::vector<std::string> split_nonempty(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i <= s.size()) {
    const std::size_t j = std::min(s.find('/', i), s.size());
    if (j > i) {
      out.emplace_back(s.substr(i, j - i));
    }
    i = j + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) {
      out += '/';
    }
    out += v[i];
  }
  return out;
}

std::string dirname(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? "." : std::string(path.substr(0, slash));
}

// Pathname#+ for relative paths.
std::string plus(std::string_view left, std::string_view right) {
  auto prefix = split_nonempty(left);
  auto suffix = split_nonempty(right);
  std::vector<std::string> kept;
  for (;;) {
    while (!suffix.empty() && suffix.front() == ".") {
      suffix.erase(suffix.begin());
    }
    if (prefix.empty()) {
      break;
    }
    std::string last = std::move(prefix.back());
    prefix.pop_back();
    if (last == ".") {
      continue;
    }
    if (last == ".." || suffix.empty() || suffix.front() != "..") {
      kept.push_back(std::move(last));
      break;
    }
    suffix.erase(suffix.begin());
  }
  for (auto& k : kept) {
    prefix.push_back(std::move(k));
  }
  for (auto& s : suffix) {
    prefix.push_back(std::move(s));
  }
  return prefix.empty() ? "." : join(prefix);
}

// Pathname#cleanpath for relative paths.
std::string cleanpath(std::string_view path) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i <= path.size()) {
    const std::size_t j = std::min(path.find('/', i), path.size());
    const std::string_view c = path.substr(i, j - i);
    if (c.empty() || c == ".") {
    } else if (c == ".." && !out.empty() && out.back() != "..") {
      out.pop_back();
    } else {
      out.emplace_back(c);
    }
    i = j + 1;
  }
  return out.empty() ? "." : join(out);
}

std::string resolve_path(std::string_view directory, std::string_view filename) {
  if (filename.starts_with("../")) {
    return cleanpath(plus(directory, filename));
  }
  if (filename.starts_with('/')) {
    return std::string(filename.substr(1));
  }
  if (filename.starts_with("./")) {
    filename.remove_prefix(2);
  }
  return plus(directory, filename);
}

bool is_word(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
         });
}

// The start of the extension that `sub(/\.(\w+(\.map)?)$/)` replaces.
std::optional<std::size_t> digestable_extension_start(std::string_view path) {
  for (std::size_t dot = path.find('.'); dot != std::string_view::npos; dot = path.find('.', dot + 1)) {
    const std::string_view rest = path.substr(dot + 1);
    if (is_word(rest) || (rest.ends_with(".map") && is_word(rest.substr(0, rest.size() - 4)))) {
      return dot;
    }
  }
  return std::nullopt;
}

// Pathname sorts "/" below every other byte.
std::string sort_key(const fs::path& p) {
  std::string s = p.string();
  std::replace(s.begin(), s.end(), '/', '\0');
  return s;
}

void all_files(const fs::path& dir, std::vector<fs::path>& files) {
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_directory()) {
      all_files(entry.path(), files);
    } else {
      files.push_back(entry.path());
    }
  }
}

// Propshaft::LoadPath#dedup
std::vector<fs::path> dedup(const std::vector<fs::path>& paths) {
  std::vector<const fs::path*> sorted;
  for (const auto& p : paths) {
    sorted.push_back(&p);
  }
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const fs::path* a, const fs::path* b) { return sort_key(*a) < sort_key(*b); });
  std::vector<std::string> kept;
  for (const auto* p : sorted) {
    const std::string s = p->string();
    if (kept.empty() || !s.starts_with(kept.back())) {
      kept.push_back(s);
    }
  }
  std::vector<fs::path> out;
  std::vector<std::string> seen;
  for (const auto& p : paths) {
    const std::string s = p.string();
    if (std::find(kept.begin(), kept.end(), s) != kept.end() && std::find(seen.begin(), seen.end(), s) == seen.end()) {
      seen.push_back(s);
      out.push_back(p);
    }
  }
  return out;
}

Result<std::string> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(Errc::Io, "cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

std::string_view extname(std::string_view path) noexcept {
  const auto slash = path.rfind('/');
  std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto first = base.find_first_not_of('.');
  if (first == std::string_view::npos) {
    return {};
  }
  base.remove_prefix(first);
  const auto dot = base.rfind('.');
  return dot == std::string_view::npos ? std::string_view{} : base.substr(dot);
}

std::string hex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    out += kDigits[b >> 4];
    out += kDigits[b & 15];
  }
  return out;
}

Result<LoadPath> LoadPath::create(const std::vector<fs::path>& dirs, std::string version, std::string prefix) {
  LoadPath lp;
  lp.version_ = std::move(version);
  lp.prefix_ = std::move(prefix);
  for (const auto& dir : dedup(dirs)) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
      continue;
    }
    std::vector<fs::path> files;
    all_files(dir, files);
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return sort_key(a) < sort_key(b); });
    for (const auto& file : files) {
      if (file.filename().string().starts_with('.')) {
        continue;
      }
      std::string logical = file.lexically_relative(dir).string();
      if (lp.by_logical_.contains(logical)) {
        continue;
      }
      auto content = read_file(file);
      if (!content) {
        return std::unexpected(content.error());
      }
      lp.by_logical_.emplace(logical, lp.assets_.size());
      lp.assets_.push_back(SourceAsset{std::move(logical), file, std::move(*content)});
    }
  }
  lp.digests_.resize(lp.assets_.size());
  return lp;
}

std::optional<std::size_t> LoadPath::find(std::string_view logical_path) const {
  const auto it = by_logical_.find(std::string(logical_path));
  if (it == by_logical_.end()) {
    return std::nullopt;
  }
  return it->second;
}

Kind LoadPath::kind(std::size_t index) const {
  const std::string_view ext = extname(assets_[index].logical_path);
  return ext == ".css" ? Kind::Css : ext == ".js" ? Kind::Js : Kind::Other;
}

void LoadPath::collect_references(std::size_t index, const Regex& pattern, std::vector<std::size_t>& out) const {
  const std::string& content = assets_[index].content;
  const std::string directory = dirname(assets_[index].logical_path);
  std::size_t pos = 0;
  for (;;) {
    auto found = pattern.search(content, pos);
    if (!found || !*found) {
      return;
    }
    const Match& m = **found;
    pos = m.groups[0].second;
    const std::string url = content.substr(m.groups[1].first, m.groups[1].second - m.groups[1].first);
    const auto referenced = find(resolve_path(directory, url));
    if (referenced && std::find(out.begin(), out.end(), *referenced) == out.end()) {
      out.push_back(*referenced);
      collect_references(*referenced, pattern, out);
    }
  }
}

const std::string& LoadPath::digest(std::size_t index) const {
  std::string& cached = digests_[index];
  if (!cached.empty()) {
    return cached;
  }
  std::vector<std::size_t> references;
  const Kind k = kind(index);
  if (k != Kind::Other) {
    collect_references(index, k == Kind::Css ? patterns().css : patterns().js, references);
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
  EVP_DigestUpdate(ctx, assets_[index].content.data(), assets_[index].content.size());
  for (const std::size_t r : references) {
    EVP_DigestUpdate(ctx, assets_[r].content.data(), assets_[r].content.size());
  }
  EVP_DigestUpdate(ctx, version_.data(), version_.size());
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_DigestFinal_ex(ctx, md, &len);
  EVP_MD_CTX_free(ctx);
  cached = hex(std::string_view(reinterpret_cast<const char*>(md), len)).substr(0, 8);
  return cached;
}

std::string LoadPath::digested_path(std::size_t index) const {
  const std::string& logical = assets_[index].logical_path;
  if (patterns().already_digested.matches(logical)) {
    return logical;
  }
  const auto dot = digestable_extension_start(logical);
  if (!dot) {
    return logical;
  }
  return logical.substr(0, *dot) + "-" + digest(index) + logical.substr(*dot);
}

Result<std::optional<std::string>> LoadPath::compiled_content(std::size_t index) const {
  const Kind k = kind(index);
  if (k == Kind::Other) {
    return std::optional<std::string>{};
  }
  const bool is_css = k == Kind::Css;
  const SourceAsset& asset = assets_[index];
  const std::string directory = dirname(asset.logical_path);
  std::string url_prefix = prefix_;
  while (url_prefix.ends_with('/')) {
    url_prefix.pop_back();
  }

  auto urls = (is_css ? patterns().css : patterns().js)
                  .gsub(asset.content, [&](const Match& m) {
                    const std::string url = asset.content.substr(m.groups[1].first, m.groups[1].second - m.groups[1].first);
                    std::string fingerprint;
                    if (m.has(2)) {
                      fingerprint = asset.content.substr(m.groups[2].first, m.groups[2].second - m.groups[2].first);
                    }
                    const auto found = find(resolve_path(directory, url));
                    std::string r = found ? "\"" + url_prefix + "/" + digested_path(*found) + fingerprint + "\""
                                          : "\"" + url + "\"";
                    return is_css ? "url(" + r + ")" : r;
                  });
  if (!urls) {
    return std::unexpected(urls.error());
  }

  auto prefix_regex = Regex::compile("^(.+/)?\\Q" + prefix_ + "\\E/", true);
  if (!prefix_regex) {
    return std::unexpected(prefix_regex.error());
  }
  const std::string& input = *urls;
  auto mapped = patterns().source_mapping.gsub(input, [&](const Match& m) {
    const auto text = [&](std::size_t i) { return input.substr(m.groups[i].first, m.groups[i].second - m.groups[i].first); };
    const std::string start = text(1);
    const std::string end = m.has(3) ? text(3) : std::string();
    auto stripped = prefix_regex->gsub(text(2), [](const Match&) { return std::string(); });
    const std::string url = stripped ? *stripped : text(2);
    const std::string resolved = directory == "." ? url : plus(directory, url);
    const auto found = find(resolved);
    if (!found) {
      return start + end;
    }
    return start + "# sourceMappingURL=" + url_prefix + "/" + digested_path(*found) + end;
  });
  if (!mapped) {
    return std::unexpected(mapped.error());
  }
  return std::optional<std::string>(std::move(*mapped));
}

}  // namespace campfire::assets::build
