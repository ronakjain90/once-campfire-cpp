// Error responses. Rails: ActionDispatch::PublicExceptions. Rust: crates/kit/src/exceptions.rs.
#include "app/errors.hpp"

#include <string>

#include "app/data.hpp"

namespace campfire::app {

std::string_view rack_reason(int status) noexcept {
  switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Internal Server Error";
  }
}

namespace {

std::string_view page_for(int status) noexcept {
  switch (status) {
    case 404: return data::f_404_html;
    case 422: return data::f_422_html;
    case 500: return data::f_500_html;
    case 502: return data::f_502_html;
    default: return {};
  }
}

std::string xml_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
  return out;
}

}  // namespace

net::Response render_error(net::Ctx& ctx, int status, req::Format format, bool head) {
  net::Response response = ctx.response(status);
  const std::string_view type = format != nullptr ? format->string : "text/html";
  const auto content_type = [&](std::string_view t) {
    response.add_copy("content-type", std::string(t) + "; charset=UTF-8");
  };
  if (head) {
    content_type(type);
    response.add("content-length", "0");
    return response;
  }
  const std::string_view symbol = format != nullptr ? format->symbol : "";
  const std::string_view reason = rack_reason(status);
  std::string body;
  bool have_body = true;
  if (symbol == "json") {
    body = "{\"status\":" + std::to_string(status) + ",\"error\":\"" + std::string(reason) + "\"}";
  } else if (symbol == "xml") {
    body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<hash>\n  <status type=\"integer\">" + std::to_string(status) +
           "</status>\n  <error>" + xml_escape(reason) + "</error>\n</hash>\n";
  } else if (symbol == "yaml") {
    body = "---\n:status: " + std::to_string(status) + "\n:error: " + std::string(reason) + "\n";
  } else {
    have_body = false;
  }
  if (have_body) {
    content_type(type);
    response.add_copy("content-length", std::to_string(body.size()));
    response.body_view(ctx.arena().copy(body));
    return response;
  }
  const std::string_view page = page_for(status);
  content_type("text/html");
  response.add_copy("content-length", std::to_string(page.size()));
  response.body_view(page);
  return response;
}

}  // namespace campfire::app
