// Inputs of the layouts (reference/app/views/layouts). A page makes a `LayoutParts` with its
// parts: `content_for :head`, `:nav`, `:footer`, `:sidebar` and the page itself (`yield`).
#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "core/out.hpp"
#include "views/context.hpp"

namespace campfire::views {

// Writes one part of a page to the output. An empty function is an empty part.
using Region = std::function<void(Out&)>;

struct LayoutParts {
  // `@page_title`; the layout uses "Campfire" if it is empty.
  std::optional<std::string_view> page_title;
  // `@body_class`.
  std::optional<std::string_view> body_class;
  Region head;
  Region nav;
  Region content;
  Region footer;
  Region sidebar;
};

namespace helpers {

// `yield :name` of a layout.
inline void region(Out& out, const Region& part) {
  if (part) {
    part(out);
  }
}

}  // namespace helpers
}  // namespace campfire::views
