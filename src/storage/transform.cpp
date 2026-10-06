// See transform.hpp.
#include "storage/transform.hpp"

#include <algorithm>
#include <limits>

#include "storage/errors.hpp"
#include "storage/marcel.hpp"
#include "storage/vips.hpp"

namespace campfire::storage {

namespace marshal = compat::marshal;

namespace {

std::unexpected<Error> invalid(std::string_view detail) {
  return prefixed(Errc::InvalidArgument, kInvalidVariation, detail);
}

struct Operation {
  std::optional<int> width;
  std::optional<int> height;
};

// Object#present? negated, for the values a transformation can hold.
bool blank(const marshal::Value& value) {
  return std::visit(
      [](const auto& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, marshal::Value::Nil>) {
          return true;
        } else if constexpr (std::is_same_v<T, bool>) {
          return !x;
        } else if constexpr (std::is_same_v<T, marshal::Value::Str>) {
          return std::all_of(x.text.begin(), x.text.end(),
                             [](unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); });
        } else if constexpr (std::is_same_v<T, marshal::Value::Array> || std::is_same_v<T, marshal::Value::Hash>) {
          return x.empty();
        } else {
          return false;
        }
      },
      value.variant());
}

Result<std::optional<int>> dimension(const marshal::Value* value) {
  if (!value || std::holds_alternative<marshal::Value::Nil>(value->variant())) return std::optional<int>();
  const auto* n = std::get_if<int64_t>(&value->variant());
  if (!n) return invalid("resize_to_limit argument");
  if (*n < std::numeric_limits<int>::min() || *n > std::numeric_limits<int>::max()) {
    return invalid("resize_to_limit argument " + std::to_string(*n));
  }
  return std::optional<int>(static_cast<int>(*n));
}

// ImageProcessingTransformer#operations: every transformation except format, skipping blank
// arguments. combine_options and any other name are errors.
Result<std::vector<Operation>> operations(const Variation& variation) {
  std::vector<Operation> result;
  for (const auto& [name, argument] : variation.transformations()) {
    if (name == "format") continue;
    if (name == "combine_options") return invalid("combine_options is not supported");
    if (blank(argument)) continue;
    const auto* args = std::get_if<marshal::Value::Array>(&argument.variant());
    if (name != "resize_to_limit" || !args || args->size() != 2) return invalid("unsupported transformation " + name);
    auto width = dimension(&(*args)[0]);
    if (!width) return std::unexpected(width.error());
    auto height = dimension(&(*args)[1]);
    if (!height) return std::unexpected(height.error());
    if (!*width && !*height) return invalid("either width or height must be specified");
    result.push_back({*width, *height});
  }
  return result;
}

}  // namespace

const marshal::Value* variation_get(const Variation& variation, std::string_view name) {
  for (const auto& [key, value] : variation.transformations()) {
    if (key == name) return &value;
  }
  return nullptr;
}

Result<std::string> variation_format(const Variation& variation) {
  std::string format = "png";
  if (const auto* value = variation_get(variation, "format")) {
    if (const auto* sym = std::get_if<marshal::Value::Symbol>(&value->variant())) {
      format = sym->name;
    } else if (const auto* str = std::get_if<marshal::Value::Str>(&value->variant())) {
      format = str->text;
    } else {
      return invalid("invalid format");
    }
  }
  if (!marcel::by_extension(format)) return invalid("invalid variant format (\"" + format + "\")");
  return format;
}

Result<std::string> variation_content_type(const Variation& variation) {
  auto format = variation_format(variation);
  if (!format) return std::unexpected(format.error());
  return marcel::for_extension(*format);
}

Variation default_to(const Variation& variation, const Transformations& defaults) {
  Transformations merged = defaults;
  for (const auto& [key, value] : variation.transformations()) {
    auto it = std::find_if(merged.begin(), merged.end(), [&](const auto& e) { return e.first == key; });
    if (it != merged.end()) {
      it->second = value;
    } else {
      merged.emplace_back(key, value);
    }
  }
  return Variation(std::move(merged));
}

Result<TempFile> transform(const std::filesystem::path& input, const Variation& variation) {
  auto format = variation_format(variation);
  if (!format) return std::unexpected(format.error());
  auto ops = operations(variation);
  if (!ops) return std::unexpected(ops.error());
  auto image = vips::Image::load_for_processing(input);
  if (!image) return std::unexpected(image.error());
  for (const auto& op : *ops) {
    auto resized = image->resize_to_limit(op.width, op.height);
    if (!resized) return std::unexpected(resized.error());
    *image = std::move(*resized);
  }
  auto output = TempFile::create("image_processing", "." + *format);
  if (!output) return std::unexpected(output.error());
  if (auto s = image->write_to_file(output->path()); !s) return std::unexpected(s.error());
  return output;
}

}  // namespace campfire::storage
