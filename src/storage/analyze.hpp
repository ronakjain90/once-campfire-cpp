// The Active Storage analyzers Campfire runs: ImageAnalyzer::Vips, VideoAnalyzer, AudioAnalyzer,
// then NullAnalyzer (Rails: activestorage/lib/active_storage/analyzer; Rust: crates/storage/src/analyze.rs).
#pragma once

#include <filesystem>
#include <string_view>

#include "compat/json.hpp"
#include "core/error.hpp"

namespace campfire::storage {

enum class Analyzer { Image, Video, Audio, Null };

Analyzer analyzer_for(std::string_view content_type);
// analyze_later?: only the null analyzer does not queue a job.
inline bool analyze_later(Analyzer a) { return a != Analyzer::Null; }

// analyzer.metadata for a local copy of the blob. Video and audio run ffprobe (30 s limit); an
// absent ffprobe gives {}.
Result<compat::json::Value> analyze_metadata(Analyzer analyzer, const std::filesystem::path& path);

// VideoAnalyzer#metadata and AudioAnalyzer#metadata for the JSON that ffprobe printed.
Result<compat::json::Value> video_metadata(const compat::json::Value& probe);
Result<compat::json::Value> audio_metadata(const compat::json::Value& probe);

}  // namespace campfire::storage
