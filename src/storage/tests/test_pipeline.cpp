// The upload, analyze, variant and preview pipeline against the reference output
// (Rust: pipeline_matches_the_reference and the staging tests in crates/storage/tests/vectors.rs).
#include <cstdio>
#include <filesystem>
#include <map>

#include "storage/key.hpp"
#include "storage/storage.hpp"
#include "storage/vips.hpp"
#include "support.hpp"

using namespace testing_support;
using namespace storage_test;
namespace st = campfire::storage;
namespace compat = campfire::compat;
namespace fs = std::filesystem;

namespace {

// Records in memory: the same rules as the SQL tables, with the unique variant index.
class MemoryRecords : public st::Records {
 public:
  struct Attachment {
    std::string name, record_type;
    int64_t record_id, blob_id;
  };
  campfire::Result<std::optional<st::Blob>> attached(std::string_view record_type, int64_t record_id,
                                               std::string_view name) override {
    for (const auto& a : attachments) {
      if (a.record_type == record_type && a.record_id == record_id && a.name == name) return std::optional(blobs.at(a.blob_id));
    }
    return std::optional<st::Blob>();
  }
  campfire::Result<st::Blob> insert_blob(const st::NewBlob& n, compat::Timestamp) override {
    st::Blob b;
    b.id = ++next_blob;
    b.key = n.key;
    b.filename = n.filename;
    b.content_type = n.content_type;
    b.metadata = n.metadata;
    b.service_name = n.service_name;
    b.byte_size = n.byte_size;
    b.checksum = n.checksum;
    blobs[b.id] = b;
    return b;
  }
  campfire::Result<int64_t> insert_attachment(std::string_view name, std::string_view record_type, int64_t record_id,
                                        int64_t blob_id, compat::Timestamp) override {
    attachments.push_back({std::string(name), std::string(record_type), record_id, blob_id});
    return int64_t(attachments.size());
  }
  campfire::Status update_metadata(int64_t blob_id, const json::Value& metadata) override {
    blobs.at(blob_id).metadata = metadata;
    return {};
  }
  campfire::Result<std::optional<int64_t>> find_variant_record(int64_t blob_id, std::string_view digest) override {
    for (const auto& [id, v] : variants) {
      if (v.first == blob_id && v.second == digest) return std::optional(id);
    }
    return std::optional<int64_t>();
  }
  campfire::Result<std::optional<int64_t>> insert_variant_record(int64_t blob_id, std::string_view digest) override {
    if (auto found = find_variant_record(blob_id, digest); found && *found) return std::optional<int64_t>();
    int64_t id = ++next_variant;
    variants[id] = {blob_id, std::string(digest)};
    return std::optional(id);
  }

  std::map<int64_t, st::Blob> blobs;
  std::vector<Attachment> attachments;
  std::map<int64_t, std::pair<int64_t, std::string>> variants;
  int64_t next_blob = 0, next_variant = 0;
};

struct TempRoot {
  fs::path path = fs::temp_directory_path() / ("cfst-" + st::generate_key());
  TempRoot() { fs::create_directories(path); }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string first_line(const std::string& command) {
  std::string out;
  if (FILE* p = popen(command.c_str(), "r")) {
    char buf[512];
    if (fgets(buf, sizeof buf, p)) out = buf;
    pclose(p);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// Counts of processed outputs that match the reference bytes. The label list is printed.
struct Tally {
  int identical = 0, different = 0;
  std::vector<std::string> different_labels;
  // Video outputs that differ from the vectors only because ffmpeg on this host draws another
  // JPEG frame than ffmpeg on the host that made the vectors (see local_preview below).
  std::vector<std::string> host_different;
};

// The bytes of the preview frame from the ffmpeg CLI, run directly with the arguments of
// ActiveStorage.video_preview_arguments. It is the oracle for this host.
std::string local_preview(const std::string& input) {
  std::string cmd = "ffmpeg -loglevel error -i '" + input +
                    "' -vf 'select=eq(n\\,0)+eq(key\\,1)+gt(scene\\,0.015),loop=loop=-1:size=2,trim=start_frame=1' "
                    "-frames:v 1 -f image2 - 2>/dev/null";
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
  }
  return out;
}

}  // namespace

TEST_CASE("storage pipeline matches the reference") {
  REQUIRE_FIXTURES();
  const auto& versions = at(vectors(), "versions");
  auto vips_version = st::vips::version();
  REQUIRE(vips_version.has_value());
  std::string ffmpeg_version = first_line("ffmpeg -version 2>/dev/null");
  bool same_vips = *vips_version == at(versions, "libvips").as_string();
  bool same_ffmpeg = ffmpeg_version == at(versions, "ffmpeg").as_string();
  std::printf("MEDIA libvips local=%s reference=%s | ffmpeg local=[%s] reference=[%s]\n", vips_version->c_str(),
              at(versions, "libvips").as_string().c_str(), ffmpeg_version.c_str(), at(versions, "ffmpeg").as_string().c_str());
  bool strict = std::getenv("CAMPFIRE_REQUIRE_MEDIA_VECTORS") != nullptr;
  if (strict) REQUIRE_MESSAGE((same_vips && same_ffmpeg), "byte comparisons would be skipped: library versions differ");

  TempRoot root;
  st::Storage storage(st::DiskService(root.path, "local"), verifier());
  MemoryRecords records;
  Tally tally;
  bool video_host_differs = false;  // the preview frame equals local ffmpeg but not the vector file

  auto compare = [&](const std::string& label, const st::Blob& actual, const json::Value& expected, bool processed, bool video) {
    CHECK_MESSAGE(actual.filename.raw() == at(expected, "filename").as_string(), label << " filename");
    CHECK_MESSAGE(actual.type() == at(expected, "content_type").as_string(), label << " content_type");
    CHECK_MESSAGE(actual.service_name == at(expected, "service_name").as_string(), label << " service_name");
    bool comparable = !processed || (video ? same_vips && same_ffmpeg : same_vips);
    if (!comparable) return;
    CHECK_MESSAGE(json::encode(actual.metadata) == at(expected, "metadata").as_string(), label << " metadata");
    if (actual.checksum == at(expected, "checksum").as_string() && actual.byte_size == *at(expected, "byte_size").to_int64()) {
      ++tally.identical;
    } else if (video && video_host_differs) {
      tally.host_different.push_back(label);
    } else {
      ++tally.different;
      tally.different_labels.push_back(label);
    }
  };

  auto check_variant = [&](const std::string& label, const st::Blob& source, const json::Value& v, const st::Blob& image, bool video) {
    CHECK_MESSAGE(variation_of(at(v, "transformations_typed")).digest() == at(v, "variation_digest").as_string(), label << " digest");
    compare(label, image, at(v, "blob"), true, video);
    std::string expected_file = read_file(std::string(CAMPFIRE_VECTORS_DIR) + "/storage/" + at(v, "file").as_string());
    CHECK_MESSAGE(st::checksum(expected_file) == at(at(v, "blob"), "checksum").as_string(), label << " vector file");
    auto actual = storage.service().download(image.key);
    REQUIRE(actual.has_value());
    CHECK_MESSAGE(st::checksum(*actual) == *image.checksum, label << " stored file");
    if (same_vips && (!video || (same_ffmpeg && !video_host_differs))) CHECK_MESSAGE(*actual == expected_file, label << " bytes");
    auto record = records.find_variant_record(source.id, at(v, "variation_digest").as_string());
    CHECK_MESSAGE((record.has_value() && record->has_value()), label << " variant record");
  };

  for (const auto& m : items(at(vectors(), "messages"))) {
    const std::string name = at(m, "fixture").as_string();
    std::string data = read_file(fixture_path(name));
    auto created = storage.create_and_upload(records, data, st::Filename(name), opt_str(at(m, "declared_type")), now());
    REQUIRE_MESSAGE(created.has_value(), name << ": " << (created ? "" : created.error().message));
    st::Blob blob = *created;
    records.insert_attachment("attachment", "Message", 1, blob.id, now());
    REQUIRE(storage.analyze(records, blob).has_value());
    compare(name, blob, at(m, "blob"), false, false);
    CHECK_MESSAGE(blob.is_variable() == at(m, "variable").as_bool(), name << " variable?");
    CHECK_MESSAGE(blob.is_previewable() == at(m, "previewable").as_bool(), name << " previewable?");

    if (blob.is_video()) {
      auto preview = storage.preview_image(records, blob, now());
      REQUIRE_MESSAGE(preview.has_value(), name << " preview: " << (preview ? "" : preview.error().message));
      {
        auto stored = storage.service().download(preview->key);
        REQUIRE(stored.has_value());
        std::string local = local_preview(fixture_path(name));
        CHECK_MESSAGE(*stored == local, name << " preview frame equals the output of the ffmpeg CLI");
        std::string vector_bytes = read_file(std::string(CAMPFIRE_VECTORS_DIR) + "/storage/" + at(at(m, "preview_image"), "file").as_string());
        video_host_differs = *stored != vector_bytes && *stored == local;
      }
      compare(name + " preview_image", *preview, at(at(m, "preview_image"), "blob"), true, true);
      for (const auto& v : items(at(m, "variants"))) {
        auto image = storage.process_preview(records, blob, variation_of(at(v, "transformations_typed")), now());
        REQUIRE_MESSAGE(image.has_value(), at(v, "label").as_string() << ": " << (image ? "" : image.error().message));
        check_variant(at(v, "label").as_string(), *preview, v, *image, true);
      }
    } else if (blob.is_variable()) {
      auto variation = storage.variation_for(blob, compat::Variation::resize_to_limit(1200, 800, std::nullopt));
      REQUIRE(variation.has_value());
      const auto& v = items(at(m, "variants"))[0];
      CHECK(*variation == variation_of(at(v, "transformations_typed")));
      auto image = storage.process_variant(records, blob, *variation, now());
      REQUIRE_MESSAGE(image.has_value(), name << ": " << (image ? "" : image.error().message));
      auto again = storage.process_variant(records, blob, *variation, now());
      REQUIRE(again.has_value());
      CHECK_MESSAGE(again->id == image->id, name << " second process reuses the record");
      check_variant(at(v, "label").as_string(), blob, v, *image, false);
    }
  }

  struct Named {
    const char* kind;
    compat::Variation first;
  };
  for (const auto& named : {Named{"avatars", compat::Variation::resize_to_limit(512, 512, "webp")},
                            Named{"logos", compat::Variation::resize_to_limit(512, 512, "png")}}) {
    for (const auto& entry : items(at(vectors(), named.kind))) {
      const auto& row = at(entry, "blob");
      std::string data = read_file(fixture_path(at(entry, "fixture").as_string()));
      auto created = storage.create_and_upload(records, data, st::Filename(at(row, "filename").as_string()),
                                               opt_str(at(row, "content_type")), now());
      REQUIRE(created.has_value());
      st::Blob blob = *created;
      REQUIRE(storage.analyze(records, blob).has_value());
      compare(at(row, "filename").as_string(), blob, row, false, false);
      size_t i = 0;
      for (const auto& v : items(at(entry, "variants"))) {
        auto transformations = i++ == 0 ? named.first : compat::Variation::resize_to_limit(192, 192, "png");
        auto variation = storage.variation_for(blob, transformations);
        REQUIRE(variation.has_value());
        CHECK_MESSAGE(*variation == variation_of(at(v, "transformations_typed")), at(v, "label").as_string());
        auto image = storage.process_variant(records, blob, *variation, now());
        REQUIRE_MESSAGE(image.has_value(), at(v, "label").as_string() << ": " << (image ? "" : image.error().message));
        check_variant(at(v, "label").as_string(), blob, v, *image, false);
      }
    }
  }

  std::printf("MEDIA byte-identical=%d different=%d\n", tally.identical, tally.different);
  std::printf("MEDIA video outputs that differ only by the ffmpeg of this host: %zu\n", tally.host_different.size());
  for (const auto& l : tally.host_different) std::printf("MEDIA host-different: %s\n", l.c_str());
  for (const auto& l : tally.different_labels) std::printf("MEDIA different: %s\n", l.c_str());
  CHECK(tally.different == 0);
  CHECK(tally.identical > 0);
}

TEST_CASE("storage staging a file gives the same blob as its bytes") {
  REQUIRE_FIXTURES();
  TempRoot root;
  st::Storage storage(st::DiskService(root.path, "local"), verifier());
  for (const auto& m : items(at(vectors(), "messages"))) {
    const std::string name = at(m, "fixture").as_string();
    auto declared = opt_str(at(m, "declared_type"));
    auto from_file = storage.stage_file(fixture_path(name), st::Filename(name), declared);
    auto from_bytes = storage.stage_bytes(read_file(fixture_path(name)), st::Filename(name), declared);
    REQUIRE(from_file.has_value());
    REQUIRE(from_bytes.has_value());
    const auto& a = from_file->blob();
    const auto& b = from_bytes->blob();
    CHECK_MESSAGE(a.content_type == b.content_type, name);
    CHECK_MESSAGE(a.checksum == b.checksum, name);
    CHECK_MESSAGE(a.byte_size == b.byte_size, name);
    for (const auto* staged : {&a, &b}) {
      auto sum = st::checksum_file(storage.service().path_for(staged->key));
      REQUIRE(sum.has_value());
      CHECK_MESSAGE(*sum == staged->checksum, name << " stored copy");
    }
  }
}

TEST_CASE("storage a staged file is deleted unless kept") {
  TempRoot root;
  st::Storage storage(st::DiskService(root.path, "local"), verifier());
  fs::path dropped_path;
  {
    auto dropped = storage.stage_bytes("dropped", st::Filename("a.txt"), std::nullopt);
    REQUIRE(dropped.has_value());
    dropped_path = storage.service().path_for(dropped->blob().key);
    CHECK(fs::exists(dropped_path));
  }
  CHECK_FALSE(fs::exists(dropped_path));
  auto kept = storage.stage_bytes("kept", st::Filename("b.txt"), std::nullopt);
  REQUIRE(kept.has_value());
  fs::path kept_path = storage.service().path_for(kept->blob().key);
  kept->keep();
  CHECK(fs::exists(kept_path));
}
