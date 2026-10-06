// Link unfurling: `UnfurlLinksController#create` over `Opengraph::Metadata`, `Location`, `Fetch` and `Document`.
// Rails: reference/app/models/opengraph/*, reference/app/controllers/unfurl_links_controller.rb. Rust:
// crates/campfire/src/integrations/opengraph.rs and opengraph/*.rs.
//
// Every address goes through the private network guard and is pinned. Every redirect is checked again. A document is
// at most 5 MB and 10 responses. Unlike Rails (60 seconds for each connect and read), an unfurl has 10 seconds in all
// and each connect or read has 5. At most 16 unfurls run at once. The endpoint is open to every signed-in user and it
// fetches the pages that they choose.
//
// The functions here block: call them on a job thread.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/unfurl_http.hpp"
#include "core/error.hpp"

namespace campfire::app::opengraph {

inline constexpr std::chrono::seconds kUnfurlDeadline{10};
inline constexpr std::size_t kMaxBodySize = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxRedirects = 10;
inline constexpr std::size_t kMaxConcurrentUnfurls = 16;

// What `UnfurlLinksController#create` answers.
struct Unfurl {
  bool has_content = false;  // `render json: opengraph` (200). Otherwise `head :no_content`.
  std::string json;
};

// The action after `params.require(:url)`. An unfurl that runs out of time unfurls nothing. The error is where Rails
// raises (a 500).
[[nodiscard]] Result<Unfurl> unfurl(const unfurl::Network& network, std::string_view url,
                                    unfurl::Clock::time_point deadline);

// `Opengraph::Document#opengraph_attributes`: from each `meta` whose `property` or `name` starts with "og:", the
// attribute with every "og:" removed, and the value its non-blank `content`, for the four keys, in this order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> opengraph_attributes(
    std::optional<std::string_view> body);

}  // namespace campfire::app::opengraph
