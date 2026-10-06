// The variation rules of ActiveStorage::Variation and ActiveStorage::Transformers::ImageProcessingTransformer
// that Campfire uses (Rust: crates/storage/src/variation.rs, process.rs transform).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "compat/variation.hpp"
#include "core/error.hpp"
#include "storage/tempfile.hpp"

namespace campfire::storage {

using Variation = compat::Variation;
using Transformations = compat::Variation::Transformations;

// transformations.fetch(:format, :png), checked against Marcel's extension table.
Result<std::string> variation_format(const Variation& variation);
// Marcel::MimeType.for(extension: format).
Result<std::string> variation_content_type(const Variation& variation);
// default_to(defaults): defaults.merge(transformations). Default keys come first.
Variation default_to(const Variation& variation, const Transformations& defaults);
// The transformation `name`, or nullptr.
const compat::marshal::Value* variation_get(const Variation& variation, std::string_view name);

// ImageProcessingTransformer#process: loads the file, applies each transformation and writes
// a temporary file named image_processing*.<format>. Only resize_to_limit exists, as in Campfire.
Result<TempFile> transform(const std::filesystem::path& input, const Variation& variation);

}  // namespace campfire::storage
