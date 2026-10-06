// Header map rules of the Rust port. Rust: the `http` crate's HeaderMap, as hyper writes it.
#include "net/front/headers.hpp"

#include <vector>

namespace campfire::net::front {

namespace {

struct Group {
  std::string_view name;
  std::vector<Header> values;
};

std::vector<Group> group_by_name(const Response& response) {
  std::vector<Group> groups;
  for (const Header& h : response.headers) {
    Group* target = nullptr;
    for (Group& g : groups) {
      if (iequals(g.name, h.name)) {
        target = &g;
        break;
      }
    }
    if (target == nullptr) {
      groups.push_back({h.name, {}});
      target = &groups.back();
    }
    target->values.push_back(h);
  }
  return groups;
}

void flatten(Response& response, const std::vector<Group>& groups) {
  response.headers.clear();
  for (const Group& g : groups) {
    for (const Header& h : g.values) response.headers.push_back(h);
  }
}

}  // namespace

bool remove_header(Response& response, std::string_view name) {
  if (!response.has(name)) return false;
  std::vector<Group> groups = group_by_name(response);
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (iequals(groups[i].name, name)) {
      if (i + 1 != groups.size()) groups[i] = std::move(groups.back());
      groups.pop_back();
      break;
    }
  }
  flatten(response, groups);
  return true;
}

void insert_header(Response& response, std::string_view name, std::string_view value) {
  if (!response.has(name)) {
    response.add(name, value);
    return;
  }
  std::vector<Group> groups = group_by_name(response);
  for (Group& g : groups) {
    if (iequals(g.name, name)) {
      g.values.assign(1, Header{g.values.front().name, value});
      break;
    }
  }
  flatten(response, groups);
}

}  // namespace campfire::net::front
