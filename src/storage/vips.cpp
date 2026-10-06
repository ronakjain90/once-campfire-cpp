// See vips.hpp.
#include "storage/vips.hpp"

#include <vips/vips.h>

#include <mutex>

#include "storage/errors.hpp"

namespace campfire::storage::vips {

namespace {

// The error buffer of libvips is shared by all threads. take_error copies and clears it.
std::string take_error() {
  std::string message = vips_error_buffer();
  vips_error_clear();
  while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) message.pop_back();
  return message;
}

std::unexpected<Error> vips_failure() { return prefixed(Errc::Internal, kVips, take_error()); }

// Introspect#optional_input in ruby-vips: construct-time inputs that are not required. A
// deprecated required input counts as optional.
bool optional_input(int flags) {
  bool required = (flags & VIPS_ARGUMENT_REQUIRED) && !(flags & VIPS_ARGUMENT_DEPRECATED);
  return (flags & VIPS_ARGUMENT_CONSTRUCT) && (flags & VIPS_ARGUMENT_INPUT) && !required;
}

// Utils.select_valid_loader_options: does the loader for `path` have an optional `page` input?
// The whole argument table is read, as ruby-vips does: asking for an argument that the loader
// does not have appends an error to the shared buffer.
bool loader_accepts_page(const char* path) {
  const char* loader = vips_foreign_find_load(path);
  if (!loader) {
    vips_error_clear();
    return false;
  }
  VipsObject* operation = VIPS_OBJECT(vips_operation_new(loader));
  if (!operation) {
    vips_error_clear();
    return false;
  }
  const char** names = nullptr;
  int* flags = nullptr;
  int n = 0;
  bool accepts = false;
  if (vips_object_get_args(operation, &names, &flags, &n) == 0) {
    for (int i = 0; i < n; ++i) {
      if (std::string_view(names[i]) == "page" && optional_input(flags[i])) accepts = true;
    }
  }
  g_object_unref(operation);
  return accepts;
}

// ImageProcessing::Vips::Processor::SHARPEN_MASK: new_from_array([[-1,-1,-1],[-1,32,-1],[-1,-1,-1]], 24).
Result<VipsImage*> sharpen_mask() {
  const double values[9] = {-1, -1, -1, -1, 32, -1, -1, -1, -1};
  VipsImage* mask = vips_image_new_matrix_from_array(3, 3, values, 9);
  if (!mask) return vips_failure();
  vips_image_set_double(mask, "scale", 24.0);
  vips_image_set_double(mask, "offset", 0.0);
  return mask;
}

}  // namespace

Status init() {
  static std::once_flag once;
  static Status result;
  std::call_once(once, [] {
    if (VIPS_INIT("campfire") != 0) {
      result = prefixed(Errc::Internal, kVips, "vips_init failed: " + take_error());
      return;
    }
    vips_block_untrusted_set(TRUE);
    vips_operation_block_set("VipsForeignLoadOpenslide", TRUE);
  });
  return result;
}

Result<std::string> version() {
  if (auto s = init(); !s) return std::unexpected(s.error());
  return std::string(vips_version_string());
}

Image& Image::operator=(Image&& o) noexcept {
  if (this != &o) {
    if (image_) g_object_unref(image_);
    image_ = o.image_;
    o.image_ = nullptr;
  }
  return *this;
}

Image::~Image() {
  if (image_) g_object_unref(image_);
}

Result<Image> Image::wrap(VipsImage* image) {
  if (!image) return vips_failure();
  return Image(image);
}

Result<Image> Image::open_sequential(const std::filesystem::path& path) {
  if (auto s = init(); !s) return std::unexpected(s.error());
  return wrap(vips_image_new_from_file(path.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr));
}

Result<Image> Image::load_for_processing(const std::filesystem::path& path) {
  if (auto s = init(); !s) return std::unexpected(s.error());
  VipsImage* raw = loader_accepts_page(path.c_str()) ? vips_image_new_from_file(path.c_str(), "page", 0, nullptr)
                                                      : vips_image_new_from_file(path.c_str(), nullptr);
  auto image = wrap(raw);
  if (!image) return image;
  return image->autorot();
}

int Image::width() const { return vips_image_get_width(image_); }
int Image::height() const { return vips_image_get_height(image_); }

std::optional<std::string> Image::get_string(const char* name) const {
  if (vips_image_get_typeof(image_, name) == 0) return std::nullopt;
  char* out = nullptr;
  if (vips_image_get_as_string(image_, name, &out) != 0) {
    vips_error_clear();
    return std::nullopt;
  }
  std::string value = out;
  g_free(out);
  return value;
}

Result<Image> Image::autorot() const {
  VipsImage* out = nullptr;
  if (vips_autorot(image_, &out, nullptr) != 0) return vips_failure();
  return wrap(out);
}

Result<Image> Image::resize_to_limit(std::optional<int> width, std::optional<int> height) const {
  constexpr int kMaxCoord = 10'000'000;
  VipsImage* thumb = nullptr;
  if (vips_thumbnail_image(image_, &thumb, width.value_or(kMaxCoord), "height", height.value_or(kMaxCoord), "size",
                           VIPS_SIZE_DOWN, "no_rotate", TRUE, nullptr) != 0) {
    return vips_failure();
  }
  auto thumbnail = wrap(thumb);
  if (!thumbnail) return thumbnail;
  auto mask = sharpen_mask();
  if (!mask) return std::unexpected(mask.error());
  Image mask_owner(*mask);
  VipsImage* sharpened = nullptr;
  if (vips_conv(thumbnail->image_, &sharpened, *mask, "precision", VIPS_PRECISION_INTEGER, nullptr) != 0) {
    return vips_failure();
  }
  return wrap(sharpened);
}

Status Image::write_to_file(const std::filesystem::path& path) const {
  if (vips_image_write_to_file(image_, path.c_str(), nullptr) != 0) return vips_failure();
  return {};
}

}  // namespace campfire::storage::vips
