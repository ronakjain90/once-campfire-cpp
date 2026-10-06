// libFuzzer target: query strings and urlencoded or JSON bodies to params (Rails: ParamBuilder).
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "req/query.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::req;
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  std::pmr::monotonic_buffer_resource arena;
  if (auto r = from_query_string(input, &arena)) {
    (void)r->to_json();
    (void)r->permit({"a", Permit::scalar_array("b"), Permit::any_hash("c"), Permit::nest("d", {"e"})}, &arena);
    (void)r->require("a");
  }
  if (auto r = from_form_body(input, &arena)) (void)r->to_json();
  if (auto r = from_json_body(input, &arena)) (void)r->to_json();
  return 0;
}
