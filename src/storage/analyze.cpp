// See analyze.hpp.
#include "storage/analyze.hpp"

#include <charconv>
#include <cmath>

#include "compat/ruby.hpp"
#include "storage/content_types.hpp"
#include "storage/errors.hpp"
#include "storage/process.hpp"
#include "storage/vips.hpp"

namespace campfire::storage {

namespace json = compat::json;

namespace {

using json::Value;

std::unexpected<Error> analyze_failure(std::string detail) { return prefixed(Errc::Internal, kAnalyze, detail); }

// ImageAnalyzer::Vips#metadata: dimensions, swapped for EXIF orientations that turn the image
// by 90 degrees. A file that libvips cannot read gives {}.
Value image_metadata(const std::filesystem::path& path) {
  auto image = vips::Image::open_sequential(path);
  if (!image) return Value(Value::Object{});
  bool rotated = false;
  if (auto orientation = image->get_string("exif-ifd0-Orientation")) {
    for (std::string_view r : {"Right-top", "Left-bottom", "Top-right", "Bottom-left"}) {
      if (orientation->find(r) != std::string::npos) rotated = true;
    }
  }
  int width = rotated ? image->height() : image->width();
  int height = rotated ? image->width() : image->height();
  return Value(Value::Object{{"width", Value(width)}, {"height", Value(height)}});
}

const Value* member(const Value* object, std::string_view name) {
  return object ? object->find(name) : nullptr;
}

// The member unless it is missing or null.
const Value* field(const Value* object, std::string_view name) {
  const Value* v = member(object, name);
  return v && !v->is_null() ? v : nullptr;
}

const Value* stream(const Value& probe, std::string_view codec_type) {
  const Value* streams = probe.find("streams");
  if (!streams || !streams->is_array()) return nullptr;
  for (const auto& s : streams->as_array()) {
    const Value* t = s.find("codec_type");
    if (t && t->get_string() && *t->get_string() == codec_type) return &s;
  }
  return nullptr;
}

bool present(const Value* s) { return s && s->is_object() && !s->as_object().empty(); }

// Ruby's Float(value) for the numbers and numeric strings that ffprobe prints.
Result<double> ruby_float(const Value& v) {
  if (auto i = v.to_int64()) return static_cast<double>(*i);
  if (v.is_double()) return v.as_double();
  if (const std::string* s = v.get_string()) {
    std::string_view t = compat::strip(*s);
    double out = 0;
    auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    if (t.empty() || ec != std::errc() || end != t.data() + t.size()) {
      return analyze_failure("invalid value for Float(): \"" + *s + "\"");
    }
    return out;
  }
  return analyze_failure("can't convert value into Float");
}

// Ruby's Integer(value).
Result<int64_t> ruby_integer(const Value& v) {
  if (auto i = v.to_int64()) return *i;
  if (v.is_double() && std::isfinite(v.as_double())) return static_cast<int64_t>(std::trunc(v.as_double()));
  if (const std::string* s = v.get_string()) {
    std::string_view t = compat::strip(*s);
    if (t.starts_with('+')) t.remove_prefix(1);
    int64_t n = 0;
    auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
    if (t.empty() || ec != std::errc() || end != t.data() + t.size()) {
      return analyze_failure("invalid value for Integer(): \"" + *s + "\"");
    }
    return n;
  }
  return analyze_failure("can't convert value into Integer");
}

}  // namespace

Analyzer analyzer_for(std::string_view type) {
  if (type.starts_with("image")) return Analyzer::Image;
  if (type.starts_with("video")) return Analyzer::Video;
  if (type.starts_with("audio")) return Analyzer::Audio;
  return Analyzer::Null;
}

Result<Value> video_metadata(const Value& probe) {
  const Value* video = stream(probe, "video");
  const Value* audio = stream(probe, "audio");

  std::optional<int64_t> angle;
  if (const Value* rotate = member(member(video, "tags"), "rotate")) {
    auto n = ruby_integer(*rotate);
    if (!n) return std::unexpected(n.error());
    angle = *n;
  } else if (const Value* list = field(video, "side_data_list"); list && list->is_array()) {
    for (const auto& d : list->as_array()) {
      const Value* type = d.find("side_data_type");
      if (!type || !type->get_string() || *type->get_string() != "Display Matrix") continue;
      if (const Value* rotation = field(&d, "rotation")) {
        auto n = ruby_integer(*rotation);
        if (!n) return std::unexpected(n.error());
        angle = *n;
      }
      break;
    }
  }

  std::optional<std::pair<int64_t, int64_t>> aspect;
  if (const Value* descriptor = field(video, "display_aspect_ratio")) {
    if (!descriptor->get_string()) return analyze_failure("display_aspect_ratio");
    std::string_view text = *descriptor->get_string();
    size_t colon = text.find(':');
    auto numerator = ruby_integer(Value(text.substr(0, colon)));
    if (!numerator) return std::unexpected(numerator.error());
    if (colon == std::string_view::npos) return analyze_failure("can't convert nil into Integer");
    auto denominator = ruby_integer(Value(text.substr(colon + 1)));
    if (!denominator) return std::unexpected(denominator.error());
    if (*numerator != 0) aspect = {*numerator, *denominator};
  }

  std::optional<double> encoded_width, encoded_height;
  if (const Value* w = field(video, "width")) {
    auto f = ruby_float(*w);
    if (!f) return std::unexpected(f.error());
    encoded_width = *f;
  }
  if (const Value* h = field(video, "height")) {
    auto f = ruby_float(*h);
    if (!f) return std::unexpected(f.error());
    encoded_height = *f;
  }
  std::optional<double> computed_height;
  if (encoded_width && aspect) {
    computed_height = *encoded_width * (static_cast<double>(aspect->second) / static_cast<double>(aspect->first));
  }
  bool rotated = angle && (*angle == 90 || *angle == 270 || *angle == -90 || *angle == -270);
  std::optional<double> width = rotated ? (computed_height ? computed_height : encoded_height) : encoded_width;
  std::optional<double> height = rotated ? encoded_width : (computed_height ? computed_height : encoded_height);

  std::optional<double> duration;
  const Value* d = field(video, "duration");
  if (!d) d = field(probe.find("format"), "duration");
  if (d) {
    auto f = ruby_float(*d);
    if (!f) return std::unexpected(f.error());
    duration = *f;
  }

  Value::Object out;
  if (width) out.emplace_back("width", Value(*width));
  if (height) out.emplace_back("height", Value(*height));
  if (duration) out.emplace_back("duration", Value(*duration));
  if (angle) out.emplace_back("angle", Value(*angle));
  if (aspect) out.emplace_back("display_aspect_ratio", Value(Value::Array{Value(aspect->first), Value(aspect->second)}));
  out.emplace_back("audio", Value(present(audio)));
  out.emplace_back("video", Value(present(video)));
  return Value(std::move(out));
}

Result<Value> audio_metadata(const Value& probe) {
  const Value* audio = stream(probe, "audio");
  Value::Object out;
  if (const Value* v = field(audio, "duration")) {
    auto f = ruby_float(*v);
    if (!f) return std::unexpected(f.error());
    out.emplace_back("duration", Value(*f));
  }
  if (const Value* v = field(audio, "bit_rate")) {
    auto n = ruby_integer(*v);
    if (!n) return std::unexpected(n.error());
    out.emplace_back("bit_rate", Value(*n));
  }
  if (const Value* v = field(audio, "sample_rate")) {
    auto n = ruby_integer(*v);
    if (!n) return std::unexpected(n.error());
    out.emplace_back("sample_rate", Value(*n));
  }
  if (const Value* v = field(audio, "tags")) out.emplace_back("tags", *v);
  return Value(std::move(out));
}

namespace {

// ffprobe -print_format json -show_streams -show_format -v error <path>. {} without ffprobe.
Result<Value> probe(const std::filesystem::path& path) {
  auto output = run_within({std::string(content_types::kFfprobe), "-print_format", "json", "-show_streams",
                            "-show_format", "-v", "error", path.string()},
                           kFfprobeTimeout, false);
  if (!output) {
    if (output.error().code == Errc::NotFound) return Value(Value::Object{});
    if (output.error().code == Errc::Timeout) return analyze_failure("ffprobe " + output.error().message);
    return std::unexpected(output.error());
  }
  auto parsed = json::parse(output->out);
  if (!parsed) return analyze_failure("ffprobe output: invalid JSON");
  return std::move(*parsed);
}

}  // namespace

Result<Value> analyze_metadata(Analyzer analyzer, const std::filesystem::path& path) {
  switch (analyzer) {
    case Analyzer::Image: return image_metadata(path);
    case Analyzer::Video:
    case Analyzer::Audio: {
      auto p = probe(path);
      if (!p) return std::unexpected(p.error());
      return analyzer == Analyzer::Video ? video_metadata(*p) : audio_metadata(*p);
    }
    case Analyzer::Null: return Value(Value::Object{});
  }
  return Value(Value::Object{});
}

}  // namespace campfire::storage
