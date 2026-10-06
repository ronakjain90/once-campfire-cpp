// See blob.hpp.
#include "storage/blob.hpp"

#include <fstream>

#include "storage/content_types.hpp"
#include "storage/errors.hpp"
#include "storage/key.hpp"
#include "storage/marcel.hpp"
#include "storage/process.hpp"

namespace campfire::storage {

bool Blob::is_variable() const {
  return content_types::is_variable(type());
}
bool Blob::is_previewable() const {
  return is_video() && ffmpeg_exists();
}

bool Blob::is_analyzed() const {
  const json::Value* v = metadata.find("analyzed");
  return v && !v->is_null() && !(v->is_bool() && !v->as_bool());
}

std::optional<double> Blob::dimension(std::string_view name) const {
  const json::Value* v = metadata.find(name);
  if (!v) return std::nullopt;
  if (auto i = v->to_int64()) return static_cast<double>(*i);
  if (v->is_double()) return v->as_double();
  return std::nullopt;
}

// Representable#format: the filename's extension when Marcel agrees it names the content type,
// otherwise the first extension registered for the content type.
std::optional<std::string> blob_format(const Blob& blob) {
  std::string_view extension = blob.filename.extension();
  if (!extension.empty() && marcel::for_extension(extension) == blob.type()) return std::string(extension);
  auto exts = marcel::extensions(blob.type());
  if (!exts.empty()) return std::string(exts.front());
  return std::nullopt;
}

std::string Blob::default_variant_format() const {
  if (content_types::is_web_image(type())) return blob_format(*this).value_or("png");
  return "png";
}

namespace {

std::optional<std::string> identify_type(std::string_view head, const Filename& filename,
                                         std::optional<std::string_view> declared_type, bool identify) {
  if (!declared_type || identify) {
    std::string sanitized = filename.sanitized();
    return marcel::identify(head, sanitized, declared_type);
  }
  return std::string(*declared_type);
}

NewBlob build(Filename filename, std::optional<std::string> content_type, std::string_view service_name,
              int64_t byte_size, std::string checksum) {
  NewBlob blob;
  blob.key = generate_key();
  blob.filename = std::move(filename);
  blob.content_type = std::move(content_type);
  blob.metadata = json::Value(json::Value::Object{{"identified", json::Value(true)}});
  blob.service_name = std::string(service_name);
  blob.byte_size = byte_size;
  blob.checksum = std::move(checksum);
  return blob;
}

}  // namespace

NewBlob NewBlob::unfurl(std::string_view data, Filename filename, std::optional<std::string_view> declared_type,
                        std::string_view service_name, bool identify) {
  auto type = identify_type(data, filename, declared_type, identify);
  return build(std::move(filename), std::move(type), service_name, static_cast<int64_t>(data.size()),
               storage::checksum(data));
}

Result<NewBlob> NewBlob::unfurl_file(const std::filesystem::path& path, Filename filename,
                                     std::optional<std::string_view> declared_type, std::string_view service_name,
                                     bool identify) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return file_not_found();
  std::string head(marcel::magic_prefix_len(), '\0');
  in.read(head.data(), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(in.gcount()));
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) return io_error("stat failed: " + path.string());
  auto sum = checksum_file(path);
  if (!sum) return std::unexpected(sum.error());
  auto type = identify_type(head, filename, declared_type, identify);
  return build(std::move(filename), std::move(type), service_name, static_cast<int64_t>(size), std::move(*sum));
}

}  // namespace campfire::storage
