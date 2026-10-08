// Rack::QueryParser / ActionDispatch::ParamBuilder; Rust: crates/kit/src/params.rs.
#include "req/query.hpp"

#include <algorithm>

#include "compat/json.hpp"

namespace campfire::req {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Splits on "&", drops leading spaces of every part after the first, skips empty parts.
std::vector<std::string_view> split_pairs(std::string_view qs) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  bool first = true;
  while (start <= qs.size()) {
    std::size_t end = qs.find('&', start);
    if (end == std::string_view::npos) end = qs.size();
    std::string_view part = qs.substr(start, end - start);
    if (!first) {
      while (part.starts_with(' ')) part.remove_prefix(1);
    }
    first = false;
    if (!part.empty()) parts.push_back(part);
    start = end + 1;
  }
  return parts;
}

ParamResult<RawPair> decode_pair(std::string_view part) {
  std::size_t eq = part.find('=');
  std::string_view k = part.substr(0, eq);
  auto key = decode_www_form_component(k);
  if (!key) return std::unexpected(key.error());
  RawPair pair;
  pair.key = std::move(*key);
  if (eq != std::string_view::npos) {
    auto value = decode_www_form_component(part.substr(eq + 1));
    if (!value) return std::unexpected(value.error());
    pair.has_value = true;
    pair.value = std::move(*value);
  }
  return pair;
}

std::string_view top_level_key(std::string_view name) {
  std::size_t start = name.size() > 1 ? name.find('[', 1) : std::string_view::npos;
  return start == std::string_view::npos ? name : name.substr(0, start);
}

std::size_t find_from(std::string_view s, char c, std::size_t from) {
  return from >= s.size() ? std::string_view::npos : s.find(c, from);
}

// What store_nested_param returns: the params hash it was given, a one-element array (a
// trailing [] below the top level), or nil (an empty key).
enum class StoredKind { Params, Array, Nil };
struct Stored {
  StoredKind kind = StoredKind::Params;
  Param array;  // when kind == Array
};

Param into_param(Stored&& s, ParamMap&& params) {
  switch (s.kind) {
    case StoredKind::Params: return Param::hash(std::move(params));
    case StoredKind::Array: return std::move(s.array);
    default: return {};
  }
}

ParamResult<ParamArray*> array_slot(ParamMap& params, std::string_view k) {
  Param* slot = params.get_mut(k);
  if (slot == nullptr || slot->is_null()) {
    params.insert(k, Param::array(params.resource()));
    slot = params.get_mut(k);
  }
  if (ParamArray* a = slot->as_array_mut()) return a;
  return param_fail(ParamErrc::Type, "expected Array (got " + std::string(ruby_class_name(*slot)) + ") for param `" +
                                         std::string(k) + "'");
}

// Ruby: key.split(/[\[\]]+/) walks the hash. A key with [] never matches.
bool params_hash_has_key(const ParamMap& hash, std::string_view key) {
  if (key.find("[]") != std::string_view::npos) return false;
  const ParamMap* current = &hash;
  std::size_t i = 0;
  while (i < key.size()) {
    while (i < key.size() && (key[i] == '[' || key[i] == ']')) ++i;
    std::size_t j = i;
    while (j < key.size() && key[j] != '[' && key[j] != ']') ++j;
    if (j == i) break;
    if (current == nullptr) return false;
    const Param* v = current->get(key.substr(i, j - i));
    if (v == nullptr) return false;
    current = v->as_hash();
    i = j;
  }
  return true;
}

ParamResult<Stored> store_nested_param(ParamMap& params, std::string_view name, Param v, std::size_t depth) {
  if (depth >= kDepthLimit) return param_fail(ParamErrc::TooDeep, "exceeded available parameter key space");
  std::pmr::memory_resource* mr = params.resource();

  std::string_view k;
  std::string_view after;
  if (depth == 0) {
    std::size_t start = find_from(name, '[', 1);
    if (start != std::string_view::npos) {
      k = name.substr(0, start);
      after = name.substr(start);
    } else {
      k = name;
    }
  } else if (name.starts_with("[]")) {
    k = "[]";
    after = name.substr(2);
  } else if (std::size_t close = name.starts_with('[') ? find_from(name, ']', 1) : std::string_view::npos;
             close != std::string_view::npos) {
    k = name.substr(1, close - 1);
    after = name.substr(close + 1);
  } else {
    k = name;  // malformed input, nested but not starting with [
  }

  if (k.empty()) return Stored{StoredKind::Nil, {}};

  if (after.empty()) {
    if (k == "[]" && depth != 0) {
      Stored s{StoredKind::Array, Param::array(mr)};
      if (!v.is_null()) s.array.as_array_mut()->push_back(std::move(v));
      return s;
    }
    params.insert(k, std::move(v));
  } else if (after == "[") {
    params.insert(name, std::move(v));
  } else if (after == "[]") {
    auto array = array_slot(params, k);
    if (!array) return std::unexpected(array.error());
    if (!v.is_null()) (*array)->push_back(std::move(v));
  } else if (after.starts_with("[]")) {
    // x[][y] puts a hash in the array. Otherwise nest what follows the [].
    std::string_view nested = after.substr(2);
    std::string_view child_key = nested;
    if (nested.size() >= 3 && nested.front() == '[' && nested.back() == ']') {
      std::string_view inner = nested.substr(1, nested.size() - 2);
      if (inner.find('[') == std::string_view::npos && inner.find(']') == std::string_view::npos) child_key = inner;
    }
    auto array = array_slot(params, k);
    if (!array) return std::unexpected(array.error());
    ParamMap* last = (*array)->empty() ? nullptr : (*array)->back().as_hash_mut();
    if (last != nullptr && !params_hash_has_key(*last, child_key)) {
      auto r = store_nested_param(*last, child_key, std::move(v), depth + 1);
      if (!r) return std::unexpected(r.error());
    } else {
      ParamMap child(mr);
      auto stored = store_nested_param(child, child_key, std::move(v), depth + 1);
      if (!stored) return std::unexpected(stored.error());
      (*array)->push_back(into_param(std::move(*stored), std::move(child)));
    }
  } else {
    ParamMap child(mr);
    if (Param* slot = params.get_mut(k)) {
      if (ParamMap* existing = slot->as_hash_mut()) {
        child = std::move(*existing);
      } else if (!slot->is_null()) {
        return param_fail(ParamErrc::Type, "expected Hash (got " + std::string(ruby_class_name(*slot)) +
                                               ") for param `" + std::string(k) + "'");
      }
    }
    auto stored = store_nested_param(child, after, std::move(v), depth + 1);
    if (!stored) return std::unexpected(stored.error());
    params.insert(k, into_param(std::move(*stored), std::move(child)));
  }
  return Stored{StoredKind::Params, {}};
}

}  // namespace

ParamResult<std::string> decode_www_form_component(std::string_view s) {
  // Most values (cookies, ids) have no `+` and no `%`: then the value is the result.
  std::size_t i = s.find_first_of("+%");
  if (i == std::string_view::npos) return std::string(s);
  std::string out;
  out.reserve(s.size());
  out.append(s.substr(0, i));
  while (i < s.size()) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
      ++i;
    } else if (c == '%') {
      if (i + 2 >= s.size()) return param_fail(ParamErrc::Invalid, "invalid %-encoding (" + std::string(s) + ")");
      int h = hex_value(s[i + 1]);
      int l = hex_value(s[i + 2]);
      if (h < 0 || l < 0) return param_fail(ParamErrc::Invalid, "invalid %-encoding (" + std::string(s) + ")");
      out.push_back(static_cast<char>(h * 16 + l));
      i += 3;
    } else {
      // The plain bytes up to the next `+` or `%`, in one copy.
      const std::size_t next = std::min(s.find_first_of("+%", i), s.size());
      out.append(s.substr(i, next - i));
      i = next;
    }
  }
  return out;
}

ParamResult<std::vector<RawPair>> query_pairs(std::string_view qs) {
  std::vector<RawPair> out;
  for (std::string_view part : split_pairs(qs)) {
    auto pair = decode_pair(part);
    if (!pair) return std::unexpected(pair.error());
    out.push_back(std::move(*pair));
  }
  return out;
}

ParamResult<std::vector<RawPair>> form_pairs(std::string_view body) {
  if (body.size() > kFormBytesizeLimit) {
    return param_fail(ParamErrc::Limit, "total query size exceeds limit (" + std::to_string(kFormBytesizeLimit) + ")");
  }
  if (body.ends_with('\0')) body.remove_suffix(1);  // Safari once appended a NUL
  std::size_t total = static_cast<std::size_t>(std::count(body.begin(), body.end(), '&')) + 1;
  if (total > kFormParamsLimit) {
    return param_fail(ParamErrc::Limit, "total number of query parameters (" + std::to_string(total) +
                                            ") exceeds limit (" + std::to_string(kFormParamsLimit) + ")");
  }
  if (!compat::json::valid_utf8(body)) return param_fail(ParamErrc::Invalid, "Invalid encoding for parameter");
  return query_pairs(body);
}

ParamResult<ParamMap> from_pairs(std::vector<RawPair> pairs, std::pmr::memory_resource* mr) {
  ParamMap params(mr);
  for (RawPair& pair : pairs) {
    if (!compat::json::valid_utf8(pair.key)) {
      return param_fail(ParamErrc::Invalid, "Invalid encoding for parameter");
    }
    if (top_level_key(pair.key).empty()) continue;
    Param value;
    if (pair.file) {
      value = Param::file(std::move(pair.file));
    } else if (pair.has_value) {
      if (!compat::json::valid_utf8(pair.value)) {
        return param_fail(ParamErrc::Invalid, "Invalid encoding for parameter: " + pair.value);
      }
      value = Param::string(mr, pair.value);
    }
    auto stored = store_nested_param(params, pair.key, std::move(value), 0);
    if (!stored) return std::unexpected(stored.error());
  }
  return params;
}

ParamResult<ParamMap> from_query_string(std::string_view qs, std::pmr::memory_resource* mr) {
  auto pairs = query_pairs(qs);
  if (!pairs) return std::unexpected(pairs.error());
  return from_pairs(std::move(*pairs), mr);
}

ParamResult<ParamMap> from_form_body(std::string_view body, std::pmr::memory_resource* mr) {
  auto pairs = form_pairs(body);
  if (!pairs) return std::unexpected(pairs.error());
  return from_pairs(std::move(*pairs), mr);
}

ParamResult<ParamMap> from_json_body(std::string_view body, std::pmr::memory_resource* mr) {
  auto doc = compat::json::parse(body);
  if (!doc) return param_fail(ParamErrc::Parse, "Error occurred while parsing request parameters");
  Param p = Param::from_json(mr, *doc);
  if (ParamMap* m = p.as_hash_mut()) return std::move(*m);
  ParamMap map(mr);
  map.insert("_json", std::move(p));
  return map;
}

}  // namespace campfire::req
