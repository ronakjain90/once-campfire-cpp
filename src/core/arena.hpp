// Per-request memory arena. Matches the "Ctx" arena in docs/architecture.md section 4.
#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace campfire {

// A bump allocator for the data of one request. The arena frees all of its memory at once, when
// `reset()` runs or when the arena is destroyed. A view or a pointer from the arena is valid
// until then. The arena is not thread-safe: one worker owns one arena.
class Arena {
 public:
  static constexpr std::size_t kDefaultBlock = 8192;

  // `initial_block` is the size of the first block. The arena grows geometrically.
  explicit Arena(std::size_t initial_block = kDefaultBlock,
                 std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
      : buffer_(initial_block, upstream) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;
  ~Arena() = default;

  // The memory resource. Give it to `std::pmr` containers: `std::pmr::vector<int> v{arena.resource()}`.
  [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &buffer_; }

  // Allocates `bytes` with the alignment `align`. The memory is not initialized.
  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
    return buffer_.allocate(bytes, align);
  }

  // Constructs a `T` in the arena. The arena never runs a destructor, so `T` must have a
  // trivial destructor. Use a `std::pmr` container for anything that owns memory.
  template <class T, class... Args>
  [[nodiscard]] T* make(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena::make never runs destructors: T must be trivially destructible");
    return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  // Copies `text` into the arena. The returned view is valid until `reset()`.
  [[nodiscard]] std::string_view copy(std::string_view text) {
    if (text.empty()) {
      return {};
    }
    char* data = static_cast<char*>(allocate(text.size(), 1));
    std::char_traits<char>::copy(data, text.data(), text.size());
    return {data, text.size()};
  }

  // Frees all memory. Every pointer and view from the arena is invalid after this call.
  void reset() { buffer_.release(); }

 private:
  std::pmr::monotonic_buffer_resource buffer_;
};

}  // namespace campfire
