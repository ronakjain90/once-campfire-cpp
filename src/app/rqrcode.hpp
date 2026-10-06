// A port of rqrcode_core 2.1.0 and rqrcode 3.2.0 `as_svg`, for QrCodeController. Rails:
// reference/app/controllers/qr_code_controller.rb. Rust: crates/campfire/src/controllers/qr_code/rqrcode.rs.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campfire::app::rqrcode {

// The modules of the code (true is dark), or nothing if the data does not fit version 40 (rqrcode raises then).
// Level H, one segment: numeric, alphanumeric or 8-bit byte, as the gem chooses.
[[nodiscard]] std::optional<std::vector<std::vector<bool>>> modules(std::string_view data);
// The version that the gem uses for `data`, or nothing if it does not fit.
[[nodiscard]] std::optional<std::size_t> version_for(std::string_view data);
// `RQRCode::QRCode.new(data).as_svg(viewbox: true, fill: :white, color: :black)`.
[[nodiscard]] std::optional<std::string> svg(std::string_view data);

}  // namespace campfire::app::rqrcode
