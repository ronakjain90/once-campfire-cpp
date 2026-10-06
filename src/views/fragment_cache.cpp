// Fragment cache hook for the `{% cache key %}` block. Rails: ActionView cache helper.
#include "views/fragment_cache.hpp"

namespace campfire::views {
namespace {

class NullFragmentCache final : public FragmentCache {
 public:
  bool read(std::string_view /*key*/, Out& /*out*/) override { return false; }
  void write(std::string_view /*key*/, std::string_view /*html*/) override {}
};

thread_local FragmentCache* g_current = nullptr;

}  // namespace

FragmentCache& null_fragment_cache() noexcept {
  static NullFragmentCache cache;
  return cache;
}

FragmentCache& fragment_cache() noexcept {
  return g_current != nullptr ? *g_current : null_fragment_cache();
}

void set_fragment_cache(FragmentCache* cache) noexcept {
  g_current = cache;
}

}  // namespace campfire::views
