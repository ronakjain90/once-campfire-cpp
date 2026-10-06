// Child processes with a time limit, and the ffmpeg preview frame
// (Rails: ActiveStorage::Previewer::VideoPreviewer; Rust: crates/storage/src/process.rs).
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace campfire::storage {

// How long ffmpeg can take to draw a preview frame. Rails sets no limit.
inline constexpr std::chrono::seconds kFfmpegTimeout{60};
// How long ffprobe can take to read the streams of a file. Rails sets no limit.
inline constexpr std::chrono::seconds kFfprobeTimeout{30};

struct ProcessOutput {
  int exit_code = 0;  // -1 when a signal ended the child
  std::string out;
  std::string err;
};

// Runs argv[0] (found in PATH) with stdin closed and stdout captured. Captures stderr only when
// `capture_stderr` is true; otherwise it goes to the parent's stderr. Kills and reaps the child
// at the timeout and returns Errc::Timeout. Errc::NotFound when the program does not exist.
Result<ProcessOutput> run_within(const std::vector<std::string>& argv, std::chrono::milliseconds timeout,
                                 bool capture_stderr);

// VideoPreviewer.accept?: `ffmpeg -version` works. The answer is kept.
bool ffmpeg_exists();

// draw_relevant_frame_from: `ffmpeg -i <input> <video_preview_arguments> -`, the bytes of stdout.
Result<std::string> video_preview(const std::filesystem::path& input);

}  // namespace campfire::storage
