// The libvips calls that ruby-vips makes for Active Storage: the Vips image analyzer and
// ImageProcessing::Vips resize_to_limit with format conversion. libvips must have the version of
// the reference image (8.16.1) for byte-identical variants (Rust: crates/storage/src/vips.rs).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"

typedef struct _VipsImage VipsImage;  // NOLINT(bugprone-reserved-identifier): the libvips name

namespace campfire::storage::vips {

// vips_init, then the loader restrictions of config/initializers/vips.rb: Vips.block_untrusted(true)
// and Vips.block("VipsForeignLoadOpenslide", true). It runs on first use. A failure is kept.
Status init();
Result<std::string> version();

// One reference to a libvips image. Released on destruction.
class Image {
 public:
  Image() = default;
  Image(Image&& o) noexcept : image_(o.image_) { o.image_ = nullptr; }
  Image& operator=(Image&& o) noexcept;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  ~Image();

  // Vips::Image.new_from_file(path, access: :sequential), as the image analyzer opens files.
  static Result<Image> open_sequential(const std::filesystem::path& path);
  // ImageProcessing::Vips::Processor.load_image(path, page: 0): `page: 0` goes only to loaders
  // that accept it, then autorot.
  static Result<Image> load_for_processing(const std::filesystem::path& path);

  int width() const;
  int height() const;
  // image.get(name) for string fields such as exif-ifd0-Orientation.
  std::optional<std::string> get_string(const char* name) const;
  Result<Image> autorot() const;
  // resize_to_limit(width, height): thumbnail_image(width, height:, size: :down, no_rotate: true)
  // then conv(SHARPEN_MASK, precision: :integer).
  Result<Image> resize_to_limit(std::optional<int> width, std::optional<int> height) const;
  // write_to_file(path): the saver and its defaults come from the extension.
  Status write_to_file(const std::filesystem::path& path) const;

 private:
  explicit Image(VipsImage* image) : image_(image) {}
  static Result<Image> wrap(VipsImage* image);
  VipsImage* image_ = nullptr;
};

}  // namespace campfire::storage::vips
