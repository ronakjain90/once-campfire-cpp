// Fragment cache hook for the `{% cache key %}` block. Rails: ActionView cache helper.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "core/out.hpp"

namespace campfire::views {

// The interface of a fragment cache. The real cache comes later. A worker installs it with
// `set_fragment_cache`. The default cache does nothing, so a `{% cache %}` block always renders.
class FragmentCache {
 public:
  FragmentCache() = default;
  FragmentCache(const FragmentCache&) = delete;
  FragmentCache& operator=(const FragmentCache&) = delete;
  virtual ~FragmentCache() = default;

  // If false, a `{% cache %}` block renders its body into the output and calls nothing else.
  [[nodiscard]] virtual bool enabled() const noexcept { return false; }
  // If the key has an entry, writes the HTML of the entry to `out` and returns true.
  virtual bool read(std::string_view key, Out& out) = 0;
  // The HTML of the entry, shared and not copied; null if there is none.
  [[nodiscard]] virtual std::shared_ptr<const std::string> get(std::string_view key) {
    (void)key;
    return nullptr;
  }
  // Stores the HTML that the body rendered.
  virtual void write(std::string_view key, std::string_view html) = 0;
};

// The cache that does nothing.
FragmentCache& null_fragment_cache() noexcept;

// The cache of the current thread. It is the null cache until `set_fragment_cache` is called.
// `cache` must outlive its use. Pass nullptr to go back to the null cache.
FragmentCache& fragment_cache() noexcept;
void set_fragment_cache(FragmentCache* cache) noexcept;

// Where a page puts the fragments of the fragment cache: the Rust port hashes the ETag of a room page from its parts
// (crates/kit/src/deflater/splice.rs). A page that wants the parts sets a recorder. The templates that write a cached
// fragment call `record` with its offset and its size in the body of the page.
class FragmentRecorder {
 public:
  FragmentRecorder() = default;
  FragmentRecorder(const FragmentRecorder&) = delete;
  FragmentRecorder& operator=(const FragmentRecorder&) = delete;
  virtual ~FragmentRecorder() = default;
  virtual void record(std::size_t offset, std::size_t size) = 0;
};
[[nodiscard]] FragmentRecorder* fragment_recorder() noexcept;
void set_fragment_recorder(FragmentRecorder* recorder) noexcept;

}  // namespace campfire::views
