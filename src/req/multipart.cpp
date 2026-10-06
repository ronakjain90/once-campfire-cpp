// multipart/form-data (Rack: Rack::Multipart::Parser; Rust: crates/kit/src/body.rs).
#include "req/multipart.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <unistd.h>

#include "compat/json.hpp"

namespace campfire::req {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

// String::from_utf8_lossy: each invalid byte becomes U+FFFD.
std::string lossy(std::string_view s) {
  if (compat::json::valid_utf8(s)) return std::string(s);
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len != 0 && i + len <= s.size() && compat::json::valid_utf8(s.substr(i, len))) {
      out.append(s.substr(i, len));
      i += len;
    } else {
      out.append("\xEF\xBF\xBD");
      ++i;
    }
  }
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Rack::Multipart::Parser#normalize_filename: unescape when every "%" is a valid escape, then
// keep the base name (Windows browsers send full paths).
std::string normalize_filename(std::string_view filename) {
  bool all_valid = true;
  for (std::size_t i = 0; i < filename.size(); ++i) {
    if (filename[i] == '%' && !(i + 2 < filename.size() && hex_value(filename[i + 1]) >= 0 &&
                                 hex_value(filename[i + 2]) >= 0)) {
      all_valid = false;
    }
  }
  std::string unescaped;
  if (all_valid) {
    for (std::size_t i = 0; i < filename.size(); ++i) {
      if (filename[i] == '%') {
        unescaped += static_cast<char>(hex_value(filename[i + 1]) * 16 + hex_value(filename[i + 2]));
        i += 2;
      } else {
        unescaped += filename[i];
      }
    }
    unescaped = lossy(unescaped);
  } else {
    unescaped = std::string(filename);
  }
  if (unescaped.empty()) return unescaped;
  const std::size_t slash = unescaped.find_last_of("/\\");
  return slash == std::string::npos ? unescaped : unescaped.substr(slash + 1);
}

std::pair<std::string_view, std::string_view> split_once(std::string_view s, char c) {
  const std::size_t i = s.find(c);
  if (i == std::string_view::npos) return {s, {}};
  return {s.substr(0, i), s.substr(i + 1)};
}

}  // namespace

std::optional<std::string> parse_boundary(std::string_view content_type) {
  auto [type, params] = split_once(content_type, ';');
  type = trim(type);
  if (lower(type).rfind("multipart/", 0) != 0) return std::nullopt;
  while (!params.empty()) {
    auto [param, rest] = split_once(params, ';');
    params = rest;
    auto [key, value] = split_once(param, '=');
    if (lower(trim(key)) != "boundary") continue;
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) return std::nullopt;
    return std::string(value);
  }
  return std::nullopt;
}

std::string PartHead::effective_name() const {
  if (name && !name->empty()) return *name;
  if (filename) return *filename;
  return content_type.value_or("text/plain") + "[]";
}

PartHead parse_disposition(std::string_view value) {
  PartHead part;
  std::optional<std::string> filename_star;
  std::string_view rest = split_once(value, ';').second;
  if (value.find(';') == std::string_view::npos) rest = {};
  while (true) {
    const std::size_t eq = rest.find('=');
    if (eq == std::string_view::npos) break;
    const std::string param = lower(trim(rest.substr(0, eq)));
    rest = rest.substr(eq + 1);
    std::string val;
    if (!rest.empty() && rest.front() == '"') {
      const std::string_view quoted = rest.substr(1);
      std::size_t end = quoted.size();
      for (std::size_t i = 0; i < quoted.size(); ++i) {
        const char c = quoted[i];
        if (c == '"') {
          end = i + 1;
          break;
        }
        if (c == '\\') {
          if (i + 1 >= quoted.size()) break;
          const char next = quoted[++i];
          if (next == '"') {
            val += '"';
          } else {
            // IE sends unescaped Windows paths in filenames: keep the backslash.
            if (param == "filename") val += '\\';
            val += next;
          }
          continue;
        }
        val += c;
      }
      rest = split_once(quoted.substr(std::min(end, quoted.size())), ';').second;
      if (quoted.substr(std::min(end, quoted.size())).find(';') == std::string_view::npos) rest = {};
    } else {
      auto [v, r] = split_once(rest, ';');
      val = std::string(trim(v));
      rest = rest.find(';') == std::string_view::npos ? std::string_view{} : r;
    }
    if (param == "name") part.name = val;
    if (param == "filename") part.filename = val;
    if (param == "filename*") filename_star = val;
  }
  if (filename_star) {
    // charset'language'percent-encoded
    std::string_view s = *filename_star;
    for (int i = 0; i < 2; ++i) {
      const std::size_t q = s.find('\'');
      s = q == std::string_view::npos ? std::string_view{} : s.substr(q + 1);
    }
    part.filename = normalize_filename(s);
  } else if (part.filename) {
    part.filename = normalize_filename(*part.filename);
  }
  return part;
}


MultipartParser::MultipartParser(std::string boundary, std::filesystem::path tmp_dir, std::pmr::memory_resource* mr,
                                 std::optional<std::uint64_t> size_limit)
    : delimiter_("\r\n--" + std::move(boundary)),
      tmp_dir_(std::move(tmp_dir)),
      mr_(mr),
      size_limit_(std::min(size_limit.value_or(kMultipartBytesizeLimit), kMultipartBytesizeLimit)),
      buf_("\r\n") {}

MultipartParser::~MultipartParser() {
  if (fd_ >= 0) ::close(fd_);
}

ParamResult<void> MultipartParser::feed(std::string_view chunk) {
  if (failed_) return std::unexpected(*failed_);
  total_ += chunk.size();
  if (total_ > size_limit_) {
    failed_ = ParamError{ParamErrc::TooLarge, "request body too large"};
    return std::unexpected(*failed_);
  }
  if (state_ == State::Done) return {};
  buf_.append(chunk);
  auto result = run();
  if (!result) failed_ = result.error();
  return result;
}

ParamResult<ParamMap> MultipartParser::finish() {
  if (failed_) return std::unexpected(*failed_);
  if (state_ != State::Done) {
    // An empty body has no params. Any other body that stops early is malformed.
    if (total_ == 0) return ParamMap(mr_);
    return param_fail(ParamErrc::Parse, "bad content body");
  }
  return from_pairs(std::move(pairs_), mr_);
}

ParamResult<void> MultipartParser::run() {
  std::size_t pos = 0;
  auto compact = [&] { buf_.erase(0, pos); };
  while (true) {
    const std::string_view rest = std::string_view(buf_).substr(pos);
    switch (state_) {
      case State::Preamble: {
        const std::size_t at = rest.find(delimiter_);
        if (at == std::string_view::npos) {
          pos += rest.size() > delimiter_.size() ? rest.size() - (delimiter_.size() - 1) : 0;
          compact();
          return {};
        }
        pos += at + delimiter_.size();
        state_ = State::AfterBoundary;
        break;
      }
      case State::AfterBoundary: {
        std::size_t skip = 0;
        while (skip < rest.size() && (rest[skip] == ' ' || rest[skip] == '\t')) ++skip;
        if (rest.size() < skip + 2) {
          pos += skip;
          compact();
          return {};
        }
        const std::string_view two = rest.substr(skip, 2);
        if (two == "--") {
          state_ = State::Done;
          buf_.clear();
          return {};
        }
        if (two != "\r\n") return param_fail(ParamErrc::Parse, "bad content body");
        pos += skip + 2;
        state_ = State::Headers;
        break;
      }
      case State::Headers: {
        if (rest.starts_with("\r\n")) {
          pos += 2;
          if (auto r = begin_part({}); !r) return r;
          state_ = State::Body;
          break;
        }
        const std::size_t at = rest.find("\r\n\r\n");
        if (at == std::string_view::npos) {
          if (rest.size() > kMultipartHeaderLimit) return param_fail(ParamErrc::Parse, "part headers too large");
          compact();
          return {};
        }
        if (at > kMultipartHeaderLimit) return param_fail(ParamErrc::Parse, "part headers too large");
        if (auto r = begin_part(rest.substr(0, at)); !r) return r;
        pos += at + 4;
        state_ = State::Body;
        break;
      }
      case State::Body: {
        const std::size_t at = rest.find(delimiter_);
        if (at == std::string_view::npos) {
          const std::size_t safe = rest.size() >= delimiter_.size() ? rest.size() - (delimiter_.size() - 1) : 0;
          if (auto r = body_data(rest.substr(0, safe)); !r) return r;
          pos += safe;
          compact();
          return {};
        }
        if (auto r = body_data(rest.substr(0, at)); !r) return r;
        if (auto r = end_part(); !r) return r;
        pos += at + delimiter_.size();
        state_ = State::AfterBoundary;
        break;
      }
      case State::Done:
        buf_.clear();
        return {};
    }
  }
}

ParamResult<void> MultipartParser::begin_part(std::string_view head_text) {
  if (++parts_ > kMultipartPartLimit) return param_fail(ParamErrc::Limit, "too many multipart parts");
  part_ = PartHead{};
  std::optional<std::string> disposition;
  std::optional<std::string> content_id;
  std::string_view lines = head_text;
  while (!lines.empty()) {
    const std::size_t eol = lines.find("\r\n");
    const std::string_view line = lines.substr(0, eol);
    lines = eol == std::string_view::npos ? std::string_view{} : lines.substr(eol + 2);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return param_fail(ParamErrc::Parse, "bad part header");
    const std::string name = lower(line.substr(0, colon));
    const std::string value = lossy(trim(line.substr(colon + 1)));
    part_.head += name + ": " + value + "\r\n";
    if (name == "content-type" && !part_.content_type) part_.content_type = value;
    if (name == "content-disposition" && !disposition) disposition = value;
    if (name == "content-id" && !content_id) content_id = value;
  }
  const auto content_type = part_.content_type;
  const auto head = part_.head;
  if (disposition) {
    part_ = parse_disposition(*disposition);
  } else {
    part_ = PartHead{};
    part_.name = content_id;
  }
  part_.content_type = content_type;
  part_.head = head;

  text_value_.clear();
  file_.reset();
  if (part_.filename && part_.filename->empty()) {
    sink_ = Sink::Discard;  // a blank filename means no file was chosen: Rack drops the part
  } else if (part_.filename) {
    if (++files_ > kMultipartFileLimit) return param_fail(ParamErrc::Limit, "too many files");
    std::string pattern = (tmp_dir_ / "RackMultipartXXXXXX").string();
    fd_ = ::mkstemp(pattern.data());
    if (fd_ < 0) return param_fail(ParamErrc::Invalid, "cannot create a temp file");
    file_ = std::make_shared<UploadedFile>();
    file_->path = pattern;
    sink_ = Sink::File;
  } else {
    sink_ = Sink::Text;
  }
  return {};
}

ParamResult<void> MultipartParser::body_data(std::string_view data) {
  switch (sink_) {
    case Sink::Discard: return {};
    case Sink::Text:
      text_ += data.size();
      if (text_ > kMultipartTextLimit) return param_fail(ParamErrc::TooLarge, "request body too large");
      text_value_.append(data);
      return {};
    case Sink::File: {
      std::size_t done = 0;
      while (done < data.size()) {
        const ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return param_fail(ParamErrc::Invalid, "cannot write a temp file");
        done += static_cast<std::size_t>(n);
      }
      file_->size += data.size();
      return {};
    }
  }
  return {};
}

ParamResult<void> MultipartParser::end_part() {
  RawPair pair;
  switch (sink_) {
    case Sink::Discard: return {};
    case Sink::Text:
      pair.key = part_.effective_name();
      pair.has_value = true;
      pair.value = std::move(text_value_);
      text_value_.clear();
      break;
    case Sink::File:
      ::close(fd_);
      fd_ = -1;
      file_->original_filename = *part_.filename;
      file_->content_type = part_.content_type;
      file_->headers = part_.head;
      pair.key = part_.effective_name();
      pair.file = std::move(file_);
      break;
  }
  sink_ = Sink::Discard;
  pairs_.push_back(std::move(pair));
  return {};
}

}  // namespace campfire::req
