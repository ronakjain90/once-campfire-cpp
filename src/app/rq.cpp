// The request context. Rails: ActionController::Base, ActionDispatch::Request. Rust: crates/kit/src/ctx.rs,
// crates/kit/src/adapter.rs (dispatch).
#include "app/rq.hpp"

#include <algorithm>
#include <filesystem>

#include "req/body.hpp"
#include "req/query.hpp"

namespace campfire::app {

namespace {

// Percent-decodes a path parameter. Axum's RawPathParams does the same: a bad escape or text that is
// not UTF-8 is an error.
std::optional<std::string> percent_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out.push_back(in[i]);
      continue;
    }
    if (i + 2 >= in.size()) return std::nullopt;
    const int hi = hex(in[i + 1]);
    const int lo = hex(in[i + 2]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  if (!compat::json::valid_utf8(out)) return std::nullopt;
  return out;
}

HttpError param_failure(const req::ParamError& error) {
  if (error.code == req::ParamErrc::TooLarge) return HttpError{ErrorKind::Status, 413, error.message};
  return HttpError{ErrorKind::BadRequest, 0, error.message};
}

}  // namespace

Rq::Rq(net::Ctx& c)
    : ctx(c), app(app::app()), worker(worker_state()), request(c.request()), info(c.request(), app.proxy) {}

Rq::~Rq() {
  worker.reader().set_scope(nullptr);
}

Flow<void> Rq::init() {
  std::vector<std::string_view> cookie_headers;
  for (const net::Header& h : request.headers) {
    if (net::iequals(h.name, "cookie")) cookie_headers.push_back(h.value);
  }
  cookies_.emplace(cookie_headers, app.secrets, *app.clock);

  std::pmr::memory_resource* mr = ctx.resource();
  // Path parameters, then query parameters, then the body: the first error wins (Rust: adapter.rs).
  req::ParamMap path_params(mr);
  std::optional<HttpError> failure;
  for (std::size_t i = 0; i < ctx.params.size(); ++i) {
    const net::Header p = ctx.params.at(i);
    auto decoded = percent_decode(p.value);
    if (!decoded) {
      if (!failure) failure = HttpError{ErrorKind::BadRequest, 0, "Invalid path parameters"};
      break;
    }
    path_params.insert(p.name, req::Param::string(mr, *decoded));
  }
  auto query = req::from_query_string(request.query, mr);
  if (!query && !failure) failure = param_failure(query.error());

  req::ParamMap request_params(mr);
  if (!request.body.empty()) {
    auto parsed = req::parse_body(request.method_text,
                                  request.has_header("content-type")
                                      ? std::optional<std::string_view>(request.header("content-type"))
                                      : std::nullopt,
                                  request.body, std::filesystem::temp_directory_path(), mr);
    if (!parsed) {
      if (!failure) failure = HttpError{ErrorKind::Status, 413, "request body too large"};
    } else {
      raw_post_ = std::move(parsed->raw);
      if (!parsed->params) {
        if (!failure) failure = param_failure(parsed->params.error());
      } else {
        request_params = std::move(*parsed->params);
      }
    }
  }
  if (failure) {
    params_storage_.emplace(mr);
    params_ = &*params_storage_;
    return std::unexpected<Failure>(Failure(std::in_place_type<HttpError>, std::move(*failure)));
  }
  // `params`: body params, then query params, then path params.
  req::ParamMap merged = std::move(request_params);
  merged.merge(*query);
  merged.merge(path_params);
  params_storage_.emplace(std::move(merged));
  params_ = &*params_storage_;
  return {};
}

req::Session& Rq::session() {
  return session_.load(*cookies_);
}

req::Flash& Rq::flash() {
  if (!flash_) {
    const compat::json::Value* stored = session().get("flash");
    flash_.emplace(req::Flash::from_session_value(stored));
  }
  return *flash_;
}

void Rq::reset_session() {
  session_.reset();
  flash_.reset();
}

req::NegotiationInput Rq::negotiation_input() const {
  req::NegotiationInput input;
  if (const auto format = params_->str("format")) {
    input.format_param = *format;
    input.has_format_param = true;
  }
  input.has_accept = request.has_header("accept");
  input.accept = request.header("accept");
  input.content_type = request.header("content-type");
  input.path = request.path;
  const std::string_view with = request.header("x-requested-with");
  std::string lower(with);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  input.xhr = lower.find("xmlhttprequest") != std::string::npos;
  return input;
}

Flow<std::span<const req::Format>> Rq::formats() {
  if (!formats_) formats_ = req::formats(negotiation_input());
  if (!*formats_) return fail_with(ErrorKind::UnknownFormat);
  return std::span<const req::Format>((*formats_)->data(), (*formats_)->size());
}

Flow<req::Format> Rq::respond_to(std::span<const req::Format> offered) {
  auto list = formats();
  if (!list) return std::unexpected(std::move(list.error()));
  req::Format chosen = req::negotiate(*list, offered);
  if (chosen == nullptr) return fail_with(ErrorKind::UnknownFormat);
  if (chosen == &req::mime::ALL) chosen = offered.empty() ? &req::mime::HTML : offered.front();
  rendered_format_ = chosen;
  return chosen;
}

req::Format Rq::rendered_format() {
  if (rendered_format_ != nullptr) return rendered_format_;
  if (!formats_) formats_ = req::formats(negotiation_input());
  if (*formats_ && !(*formats_)->empty() && (*formats_)->front() != nullptr &&
      (*formats_)->front() != &req::mime::ALL) {
    return (*formats_)->front();
  }
  return &req::mime::HTML;
}

bool Rq::is_turbo_frame_request() const {
  std::string_view id = request.header("turbo-frame");
  while (!id.empty() && (id.front() == ' ' || id.front() == '\t')) id.remove_prefix(1);
  return !id.empty();
}

db::DependencyScope& Rq::track() {
  if (!scope_) scope_.emplace();
  scope_->reset();
  worker.reader().set_scope(&*scope_);
  return *scope_;
}

void Rq::set_header(std::string_view name, std::string_view value) {
  for (net::Header& h : staged_) {
    if (net::iequals(h.name, name)) {
      h.value = copy(value);
      return;
    }
  }
  staged_.push_back({copy(name), copy(value)});
}

std::string_view Rq::staged_header(std::string_view name) const noexcept {
  for (const net::Header& h : staged_) {
    if (net::iequals(h.name, name)) return h.value;
  }
  return {};
}

}  // namespace campfire::app
