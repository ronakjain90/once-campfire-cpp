// Rails request parameters: the value type behind params (Rails: ActionController::Parameters,
// ActionDispatch::ParamBuilder; Rust: crates/kit/src/params.rs). Values live in a memory
// resource, normally the arena of the request.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "compat/json.hpp"

namespace campfire::req {

// Why a request cannot make params. `Missing` is ActionController::ParameterMissing.
enum class ParamErrc : std::uint8_t {
  Type,     // ParameterTypeError: a=1&a[b]=2
  Invalid,  // InvalidParameterError: bad %-encoding or invalid UTF-8
  TooDeep,  // ParamsTooDeepError
  Limit,    // QueryLimitError
  Parse,    // Http::Parameters::ParseError: malformed JSON or multipart
  Missing,  // ParameterMissing
  TooLarge,  // a body over a limit: the server answers 413
};

struct ParamError {
  ParamErrc code = ParamErrc::Parse;
  std::string message;
  [[nodiscard]] bool operator==(const ParamError&) const = default;
};

template <class T>
using ParamResult = std::expected<T, ParamError>;

[[nodiscard]] inline std::unexpected<ParamError> param_fail(ParamErrc code, std::string message) {
  return std::unexpected<ParamError>(ParamError{code, std::move(message)});
}

// A file part of a multipart body (ActionDispatch::Http::UploadedFile). The file is a temp
// file. The destructor deletes it.
struct UploadedFile {
  std::string original_filename;
  std::optional<std::string> content_type;
  std::string headers;  // the raw part headers
  std::uint64_t size = 0;
  std::filesystem::path path;

  UploadedFile() = default;
  UploadedFile(const UploadedFile&) = delete;
  UploadedFile& operator=(const UploadedFile&) = delete;
  ~UploadedFile();

  // Writes `bytes` to a new temp file in `directory`. For tests and for raw-body attachments.
  [[nodiscard]] static std::shared_ptr<UploadedFile> from_bytes(const std::filesystem::path& directory,
                                                                std::string_view filename,
                                                                std::optional<std::string_view> content_type,
                                                                std::string_view bytes);
  [[nodiscard]] std::optional<std::string> read() const;
};

class Param;
class ParamMap;
using ParamArray = std::pmr::vector<Param>;

enum class Permitted : std::uint8_t { Key, ScalarArray, AnyHash, Nested };

// A permit filter: `:name`, `name: []`, `name: {}`, `name: [...]`.
struct Permit {
  Permitted kind = Permitted::Key;
  std::string name;
  std::vector<Permit> nested;

  Permit(const char* key) : name(key) {}  // NOLINT: `permit("a", "b")`
  Permit(std::string key) : name(std::move(key)) {}
  Permit(Permitted k, std::string key, std::vector<Permit> inner = {})
      : kind(k), name(std::move(key)), nested(std::move(inner)) {}
  [[nodiscard]] static Permit scalar_array(std::string key) { return {Permitted::ScalarArray, std::move(key)}; }
  [[nodiscard]] static Permit any_hash(std::string key) { return {Permitted::AnyHash, std::move(key)}; }
  [[nodiscard]] static Permit nest(std::string key, std::vector<Permit> inner) {
    return {Permitted::Nested, std::move(key), std::move(inner)};
  }
};

enum class ParamKind : std::uint8_t { Null, Bool, Int, UInt, Double, String, File, Array, Hash };

// An insertion ordered map from string to Param, as the Ruby Hash behind params. It keeps an
// index when it is big, so a long query string costs linear time.
class ParamMap {
 public:
  explicit ParamMap(std::pmr::memory_resource* mr = std::pmr::get_default_resource());
  ParamMap(ParamMap&&) noexcept;
  ParamMap& operator=(ParamMap&&) noexcept;
  ParamMap(const ParamMap&) = delete;
  ParamMap& operator=(const ParamMap&) = delete;
  ~ParamMap();

  [[nodiscard]] std::pmr::memory_resource* resource() const { return mr_; }
  [[nodiscard]] const Param* get(std::string_view key) const;
  [[nodiscard]] Param* get_mut(std::string_view key);
  [[nodiscard]] std::optional<std::string_view> str(std::string_view key) const;
  [[nodiscard]] bool contains(std::string_view key) const { return get(key) != nullptr; }
  // Hash#[]=: replaces in place and keeps the position.
  void insert(std::string_view key, Param value);
  void remove(std::string_view key);
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const { return size() == 0; }
  // The key and the value of the entry at `index`, in insertion order.
  [[nodiscard]] std::string_view key_at(std::size_t index) const;
  [[nodiscard]] const Param& value_at(std::size_t index) const;
  [[nodiscard]] Param& value_at(std::size_t index);

  [[nodiscard]] ParamMap clone(std::pmr::memory_resource* mr) const;
  // Hash#merge!: later values win, an old key keeps its position.
  void merge(const ParamMap& other);
  // params.require(:key): the value if it is present (or false), else Missing.
  [[nodiscard]] ParamResult<const Param*> require(std::string_view key) const;
  // params.permit(*filters) for the filter shapes Campfire uses.
  [[nodiscard]] ParamMap permit(const std::vector<Permit>& filters, std::pmr::memory_resource* mr = nullptr) const;
  [[nodiscard]] compat::json::Value to_json() const;

 private:
  struct Entry;
  struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  void rebuild_index();

  std::pmr::memory_resource* mr_;
  std::pmr::vector<Entry> entries_;
  std::pmr::unordered_map<std::pmr::string, std::size_t, KeyHash, std::equal_to<>> index_;
  bool indexed_ = false;
};

// A param value: what a Rails params hash can hold.
class Param {
 public:
  Param() = default;  // nil
  [[nodiscard]] static Param boolean(bool b);
  [[nodiscard]] static Param integer(std::int64_t n);
  [[nodiscard]] static Param unsigned_integer(std::uint64_t n);
  [[nodiscard]] static Param real(double d);
  [[nodiscard]] static Param string(std::pmr::memory_resource* mr, std::string_view s);
  [[nodiscard]] static Param file(std::shared_ptr<UploadedFile> f);
  [[nodiscard]] static Param array(std::pmr::memory_resource* mr);
  [[nodiscard]] static Param hash(ParamMap m);
  // ParamBuilder.from_hash for a decoded JSON body: nil is dropped from arrays (deep munge).
  [[nodiscard]] static Param from_json(std::pmr::memory_resource* mr, const compat::json::Value& v);

  Param(Param&&) noexcept = default;
  Param& operator=(Param&&) noexcept = default;
  Param(const Param&) = delete;
  Param& operator=(const Param&) = delete;
  ~Param() = default;

  [[nodiscard]] ParamKind kind() const;
  [[nodiscard]] bool is_null() const { return kind() == ParamKind::Null; }
  [[nodiscard]] std::optional<std::string_view> as_str() const;
  [[nodiscard]] const ParamMap* as_hash() const;
  [[nodiscard]] ParamMap* as_hash_mut();
  [[nodiscard]] const ParamArray* as_array() const;
  [[nodiscard]] ParamArray* as_array_mut();
  [[nodiscard]] const std::shared_ptr<UploadedFile>* as_file() const;
  // params[:a][:b]: nullptr for anything that is not a hash.
  [[nodiscard]] const Param* get(std::string_view key) const;
  // The value as Rails does to_s: nullopt for an array, a hash or a file.
  [[nodiscard]] std::optional<std::string> to_s() const;
  // ActiveSupport blank?: nil, false, a string of spaces, an empty array or hash.
  [[nodiscard]] bool is_blank() const;
  [[nodiscard]] bool is_present() const { return !is_blank(); }
  [[nodiscard]] bool is_permitted_scalar() const;
  [[nodiscard]] Param clone(std::pmr::memory_resource* mr) const;
  [[nodiscard]] compat::json::Value to_json() const;

 private:
  using Data = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::pmr::string,
                            std::shared_ptr<UploadedFile>, ParamArray, ParamMap>;
  explicit Param(Data d) : data_(std::move(d)) {}
  Data data_;
};

// The name of the Ruby class of a value, for error messages.
[[nodiscard]] std::string_view ruby_class_name(const Param& p);

}  // namespace campfire::req
