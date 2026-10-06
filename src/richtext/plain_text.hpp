// ActionText::PlainTextConversion. Rust: crates/richtext/src/plain_text.rs
#pragma once

#include <string>

#include "richtext/dom.hpp"

namespace campfire::richtext {

// `PlainTextConversion.node_to_plain_text`: a bottom-up reduction keyed on the name of each node.
[[nodiscard]] std::string node_to_plain_text(const Node* node);

}  // namespace campfire::richtext
