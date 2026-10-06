// Fragment cache hook for the `{% cache key %}` block. Rails: ActionView cache helper.
#pragma once

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
  // Stores the HTML that the body rendered.
  virtual void write(std::string_view key, std::string_view html) = 0;
};

// The cache that does nothing.
FragmentCache& null_fragment_cache() noexcept;

// The cache of the current thread. It is the null cache until `set_fragment_cache` is called.
// `cache` must outlive its use. Pass nullptr to go back to the null cache.
FragmentCache& fragment_cache() noexcept;
void set_fragment_cache(FragmentCache* cache) noexcept;

}  // namespace campfire::views
