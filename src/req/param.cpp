// Rails ActionController::Parameters subset and ParamBuilder values; Rust: crates/kit/src/params.rs.
#include "req/param.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "compat/ruby.hpp"

namespace campfire::req {

struct ParamMap::Entry {
  std::pmr::string key;
  Param value;
};

// ---- UploadedFile ----

UploadedFile::~UploadedFile() {
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

std::shared_ptr<UploadedFile> UploadedFile::from_bytes(const std::filesystem::path& directory,
                                                       std::string_view filename,
                                                       std::optional<std::string_view> content_type,
                                                       std::string_view bytes) {
  std::string pattern = (directory / "RackMultipartXXXXXX").string();
  int fd = ::mkstemp(pattern.data());
  if (fd < 0) return nullptr;
  auto file = std::make_shared<UploadedFile>();
  file->path = pattern;
  file->original_filename = std::string(filename);
  if (content_type) file->content_type = std::string(*content_type);
  file->size = bytes.size();
  std::size_t done = 0;
  while (done < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (done != bytes.size()) return nullptr;
  return file;
}

std::optional<std::string> UploadedFile::read() const {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// ---- ParamMap ----

ParamMap::ParamMap(std::pmr::memory_resource* mr) : mr_(mr), entries_(mr), index_(mr) {}
ParamMap::ParamMap(ParamMap&&) noexcept = default;
ParamMap& ParamMap::operator=(ParamMap&&) noexcept = default;
ParamMap::~ParamMap() = default;

namespace {
constexpr std::size_t kIndexThreshold = 16;
}

void ParamMap::rebuild_index() {
  index_.clear();
  for (std::size_t i = 0; i < entries_.size(); ++i) index_.emplace(entries_[i].key, i);
  indexed_ = true;
}

const Param* ParamMap::get(std::string_view key) const {
  if (indexed_) {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].value;
  }
  for (const Entry& e : entries_) {
    if (std::string_view(e.key) == key) return &e.value;
  }
  return nullptr;
}

Param* ParamMap::get_mut(std::string_view key) {
  return const_cast<Param*>(std::as_const(*this).get(key));
}

std::optional<std::string_view> ParamMap::str(std::string_view key) const {
  const Param* p = get(key);
  return p ? p->as_str() : std::nullopt;
}

void ParamMap::insert(std::string_view key, Param value) {
  if (Param* slot = get_mut(key)) {
    *slot = std::move(value);
    return;
  }
  entries_.push_back(Entry{std::pmr::string(key, mr_), std::move(value)});
  if (indexed_) {
    index_.emplace(entries_.back().key, entries_.size() - 1);
  } else if (entries_.size() > kIndexThreshold) {
    rebuild_index();
  }
}

void ParamMap::remove(std::string_view key) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (std::string_view(entries_[i].key) == key) {
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
      if (indexed_) rebuild_index();
      return;
    }
  }
}

std::size_t ParamMap::size() const {
  return entries_.size();
}

std::string_view ParamMap::key_at(std::size_t index) const {
  return entries_[index].key;
}
const Param& ParamMap::value_at(std::size_t index) const {
  return entries_[index].value;
}
Param& ParamMap::value_at(std::size_t index) {
  return entries_[index].value;
}

ParamMap ParamMap::clone(std::pmr::memory_resource* mr) const {
  ParamMap out(mr);
  for (const Entry& e : entries_) out.insert(e.key, e.value.clone(mr));
  return out;
}

void ParamMap::merge(const ParamMap& other) {
  for (const Entry& e : other.entries_) insert(e.key, e.value.clone(mr_));
}

ParamResult<const Param*> ParamMap::require(std::string_view key) const {
  const Param* p = get(key);
  if (p != nullptr && (p->is_present() || p->kind() == ParamKind::Bool)) return p;
  return param_fail(ParamErrc::Missing, "param is missing or the value is empty or invalid: " + std::string(key));
}

compat::json::Value ParamMap::to_json() const {
  compat::json::Value::Object obj;
  for (const Entry& e : entries_) obj.emplace_back(std::string(e.key), e.value.to_json());
  return compat::json::Value(std::move(obj));
}

namespace {

// /\A#{key}\(\d+[if]?\)\z/
bool is_multi_parameter_key(std::string_view candidate, std::string_view key) {
  if (!candidate.starts_with(key)) return false;
  std::string_view rest = candidate.substr(key.size());
  if (rest.size() < 3 || rest.front() != '(' || rest.back() != ')') return false;
  rest = rest.substr(1, rest.size() - 2);
  if (rest.back() == 'i' || rest.back() == 'f') rest.remove_suffix(1);
  return !rest.empty() && std::ranges::all_of(rest, [](char c) { return c >= '0' && c <= '9'; });
}

bool is_fields_for_style(const ParamMap& map) {
  if (map.empty()) return false;
  for (std::size_t i = 0; i < map.size(); ++i) {
    std::string_view k = map.key_at(i);
    if (k.starts_with('-')) k.remove_prefix(1);
    if (k.empty() || !std::ranges::all_of(k, [](char c) { return c >= '0' && c <= '9'; })) return false;
    if (map.value_at(i).as_hash() == nullptr) return false;
  }
  return true;
}

// permit(key: {}): any scalar, array of scalars or hash, deep.
ParamMap permit_any(const ParamMap& map, std::pmr::memory_resource* mr) {
  ParamMap out(mr);
  for (std::size_t i = 0; i < map.size(); ++i) {
    const Param& v = map.value_at(i);
    if (const ParamMap* inner = v.as_hash()) {
      out.insert(map.key_at(i), Param::hash(permit_any(*inner, mr)));
    } else if (const ParamArray* items = v.as_array()) {
      Param kept = Param::array(mr);
      for (const Param& item : *items) {
        if (const ParamMap* h = item.as_hash()) {
          kept.as_array_mut()->push_back(Param::hash(permit_any(*h, mr)));
        } else if (item.as_array() == nullptr) {
          kept.as_array_mut()->push_back(item.clone(mr));
        }
      }
      out.insert(map.key_at(i), std::move(kept));
    } else {
      out.insert(map.key_at(i), v.clone(mr));
    }
  }
  return out;
}

}  // namespace

ParamMap ParamMap::permit(const std::vector<Permit>& filters, std::pmr::memory_resource* mr_in) const {
  std::pmr::memory_resource* mr = mr_in != nullptr ? mr_in : mr_;
  ParamMap out(mr);
  for (const Permit& f : filters) {
    switch (f.kind) {
      case Permitted::Key: {
        const Param* v = get(f.name);
        if (v != nullptr && v->is_permitted_scalar()) out.insert(f.name, v->clone(mr));
        // Multi-parameter attributes, such as born_on(1i).
        for (const Entry& e : entries_) {
          if (is_multi_parameter_key(e.key, f.name) && e.value.is_permitted_scalar())
            out.insert(e.key, e.value.clone(mr));
        }
        break;
      }
      case Permitted::ScalarArray: {
        const Param* v = get(f.name);
        const ParamArray* items = v ? v->as_array() : nullptr;
        if (items != nullptr && std::ranges::all_of(*items, [](const Param& p) { return p.is_permitted_scalar(); })) {
          out.insert(f.name, v->clone(mr));
        }
        break;
      }
      case Permitted::AnyHash: {
        const Param* v = get(f.name);
        if (const ParamMap* h = v ? v->as_hash() : nullptr) out.insert(f.name, Param::hash(permit_any(*h, mr)));
        break;
      }
      case Permitted::Nested: {
        const Param* v = get(f.name);
        if (v == nullptr) break;
        if (const ParamMap* h = v->as_hash()) {
          if (is_fields_for_style(*h)) {
            ParamMap each(mr);
            for (std::size_t i = 0; i < h->size(); ++i) {
              each.insert(h->key_at(i), Param::hash(h->value_at(i).as_hash()->permit(f.nested, mr)));
            }
            out.insert(f.name, Param::hash(std::move(each)));
          } else {
            out.insert(f.name, Param::hash(h->permit(f.nested, mr)));
          }
        } else if (const ParamArray* items = v->as_array()) {
          Param hashes = Param::array(mr);
          for (const Param& item : *items) {
            if (const ParamMap* inner = item.as_hash())
              hashes.as_array_mut()->push_back(Param::hash(inner->permit(f.nested, mr)));
          }
          out.insert(f.name, std::move(hashes));
        }
        break;
      }
    }
  }
  return out;
}

// ---- Param ----

Param Param::boolean(bool b) {
  return Param(Data(b));
}
Param Param::integer(std::int64_t n) {
  return Param(Data(n));
}
Param Param::unsigned_integer(std::uint64_t n) {
  return Param(Data(n));
}
Param Param::real(double d) {
  return Param(Data(d));
}
Param Param::string(std::pmr::memory_resource* mr, std::string_view s) {
  return Param(Data(std::in_place_type<std::pmr::string>, s, mr));
}
Param Param::file(std::shared_ptr<UploadedFile> f) {
  return Param(Data(std::move(f)));
}
Param Param::array(std::pmr::memory_resource* mr) {
  return Param(Data(std::in_place_type<ParamArray>, mr));
}
Param Param::hash(ParamMap m) {
  return Param(Data(std::move(m)));
}

Param Param::from_json(std::pmr::memory_resource* mr, const compat::json::Value& v) {
  if (v.is_null()) return {};
  if (v.is_bool()) return boolean(v.as_bool());
  if (auto n = v.to_int64()) return integer(*n);
  if (const std::uint64_t* u = v.get_uint()) return unsigned_integer(*u);
  if (v.is_double()) return real(v.as_double());
  if (const std::string* s = v.get_string()) return string(mr, *s);
  if (v.is_array()) {
    Param out = array(mr);
    for (const auto& item : v.as_array()) {
      if (!item.is_null()) out.as_array_mut()->push_back(from_json(mr, item));
    }
    return out;
  }
  ParamMap map(mr);
  for (const auto& [k, item] : v.as_object()) map.insert(k, from_json(mr, item));
  return hash(std::move(map));
}

ParamKind Param::kind() const {
  switch (data_.index()) {
    case 0: return ParamKind::Null;
    case 1: return ParamKind::Bool;
    case 2: return ParamKind::Int;
    case 3: return ParamKind::UInt;
    case 4: return ParamKind::Double;
    case 5: return ParamKind::String;
    case 6: return ParamKind::File;
    case 7: return ParamKind::Array;
    default: return ParamKind::Hash;
  }
}

std::optional<std::string_view> Param::as_str() const {
  if (auto* s = std::get_if<std::pmr::string>(&data_)) return std::string_view(*s);
  return std::nullopt;
}
const ParamMap* Param::as_hash() const {
  return std::get_if<ParamMap>(&data_);
}
ParamMap* Param::as_hash_mut() {
  return std::get_if<ParamMap>(&data_);
}
const ParamArray* Param::as_array() const {
  return std::get_if<ParamArray>(&data_);
}
ParamArray* Param::as_array_mut() {
  return std::get_if<ParamArray>(&data_);
}
const std::shared_ptr<UploadedFile>* Param::as_file() const {
  return std::get_if<std::shared_ptr<UploadedFile>>(&data_);
}

const Param* Param::get(std::string_view key) const {
  const ParamMap* m = as_hash();
  return m ? m->get(key) : nullptr;
}

std::optional<std::string> Param::to_s() const {
  switch (kind()) {
    case ParamKind::Null: return std::string();
    case ParamKind::Bool: return std::string(std::get<bool>(data_) ? "true" : "false");
    case ParamKind::Int: return std::to_string(std::get<std::int64_t>(data_));
    case ParamKind::UInt: return std::to_string(std::get<std::uint64_t>(data_));
    case ParamKind::Double: return compat::float_to_s(std::get<double>(data_));
    case ParamKind::String: return std::string(std::get<std::pmr::string>(data_));
    default: return std::nullopt;
  }
}

bool Param::is_blank() const {
  switch (kind()) {
    case ParamKind::Null: return true;
    case ParamKind::Bool: return !std::get<bool>(data_);
    case ParamKind::String: {
      // String#blank?: only Unicode whitespace. ASCII whitespace and U+0085, U+00A0 and the
      // spaces of the General Punctuation block count.
      std::string_view s = std::get<std::pmr::string>(data_);
      for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || (c >= 9 && c <= 13)) {
          ++i;
        } else if (c == 0xC2 && i + 1 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) == 0x85 || static_cast<unsigned char>(s[i + 1]) == 0xA0)) {
          i += 2;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   ((static_cast<unsigned char>(s[i + 2]) >= 0x80 && static_cast<unsigned char>(s[i + 2]) <= 0x8A) ||
                    static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9 ||
                    static_cast<unsigned char>(s[i + 2]) == 0xAF)) {
          i += 3;
        } else if (c == 0xE1 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x9A &&
                   static_cast<unsigned char>(s[i + 2]) == 0x80) {
          i += 3;  // U+1680
        } else if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(s[i + 2]) == 0x80) {
          i += 3;  // U+3000
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x81 &&
                   static_cast<unsigned char>(s[i + 2]) == 0x9F) {
          i += 3;  // U+205F
        } else {
          return false;
        }
      }
      return true;
    }
    case ParamKind::Array: return std::get<ParamArray>(data_).empty();
    case ParamKind::Hash: return std::get<ParamMap>(data_).empty();
    default: return false;
  }
}

bool Param::is_permitted_scalar() const {
  return kind() != ParamKind::Array && kind() != ParamKind::Hash;
}

Param Param::clone(std::pmr::memory_resource* mr) const {
  switch (kind()) {
    case ParamKind::Null: return {};
    case ParamKind::Bool: return boolean(std::get<bool>(data_));
    case ParamKind::Int: return integer(std::get<std::int64_t>(data_));
    case ParamKind::UInt: return unsigned_integer(std::get<std::uint64_t>(data_));
    case ParamKind::Double: return real(std::get<double>(data_));
    case ParamKind::String: return string(mr, std::get<std::pmr::string>(data_));
    case ParamKind::File: return file(std::get<std::shared_ptr<UploadedFile>>(data_));
    case ParamKind::Array: {
      Param out = array(mr);
      for (const Param& p : std::get<ParamArray>(data_)) out.as_array_mut()->push_back(p.clone(mr));
      return out;
    }
    default: return hash(std::get<ParamMap>(data_).clone(mr));
  }
}

compat::json::Value Param::to_json() const {
  using compat::json::Value;
  switch (kind()) {
    case ParamKind::Null: return {};
    case ParamKind::Bool: return Value(std::get<bool>(data_));
    case ParamKind::Int: return Value(std::get<std::int64_t>(data_));
    case ParamKind::UInt: return Value::from_unsigned(std::get<std::uint64_t>(data_));
    case ParamKind::Double: return Value(std::get<double>(data_));
    case ParamKind::String: return Value(std::string(std::get<std::pmr::string>(data_)));
    case ParamKind::File: {
      const auto& f = *std::get<std::shared_ptr<UploadedFile>>(data_);
      Value::Object obj;
      obj.emplace_back("original_filename", Value(f.original_filename));
      obj.emplace_back("content_type", f.content_type ? Value(*f.content_type) : Value());
      return Value(std::move(obj));
    }
    case ParamKind::Array: {
      Value::Array arr;
      for (const Param& p : std::get<ParamArray>(data_)) arr.push_back(p.to_json());
      return Value(std::move(arr));
    }
    default: return std::get<ParamMap>(data_).to_json();
  }
}

std::string_view ruby_class_name(const Param& p) {
  switch (p.kind()) {
    case ParamKind::Null: return "NilClass";
    case ParamKind::Bool: return *p.to_s() == "true" ? "TrueClass" : "FalseClass";
    case ParamKind::Int:
    case ParamKind::UInt: return "Integer";
    case ParamKind::Double: return "Float";
    case ParamKind::String: return "String";
    case ParamKind::File: return "ActionDispatch::Http::UploadedFile";
    case ParamKind::Array: return "Array";
    default: return "ActiveSupport::HashWithIndifferentAccess";
  }
}

}  // namespace campfire::req
