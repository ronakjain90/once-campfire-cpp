// Unknown route. Rails: ActionDispatch::PublicExceptions (public/404.html); Rust: crates/kit/src/exceptions.rs.
#include "app/not_found.hpp"

#include "app/compress.hpp"
#include "app/data.hpp"
#include "app/errors.hpp"
#include "app/rails.hpp"
#include "req/format.hpp"

namespace campfire::app {

namespace {

// `request.formats.first`: the format of the extension of the path or of the Accept header. A request whose formats
// cannot be read falls back to HTML (a null format).
req::Format first_format(const net::Request& request) {
  req::NegotiationInput input;
  input.has_accept = request.has_header("accept");
  input.accept = request.header("accept");
  input.content_type = request.header("content-type");
  input.path = request.path;
  const auto formats = req::formats(input);
  if (!formats || formats->empty()) return nullptr;
  return formats->front();
}

bool has_structured_body(req::Format format) {
  return format != nullptr && (format->symbol == "json" || format->symbol == "xml" || format->symbol == "yaml");
}

}  // namespace

campfire::Task<net::Response> not_found(net::Ctx& ctx) {
  const net::Request& request = ctx.request();
  const bool head = request.method == net::Method::Head;
  // The "content-length: 0" of a HEAD answer keeps Rack::Deflater from touching it.
  if (!head && refuses_every_encoding(request)) co_return not_acceptable(ctx);
  const auto finish = [&](net::Response response) {
    add_rails_tail(ctx, response);
    add_hsts(request, response);
    // Rack::Deflater adds Vary to a body that it may compress (not to the empty body of HEAD).
    if (!head) response.add("vary", "Accept-Encoding");
    return response;
  };
  const req::Format format = first_format(request);
  if (head || has_structured_body(format)) {
    // JSON, XML and YAML get the `{ status:, error: }` hash. HEAD gets an empty body in the format of the request,
    // `*/*` included (Rust: exceptions.rs).
    net::Response response = render_error(ctx, 404, format, head);
    if (!head && wants_gzip(request)) {
      // Rack::Deflater: the body is compressed, and `Content-Length` goes (the Rust port sends it chunked).
      const std::string packed = gzip_compress(*response.body_contiguous());
      response.erase("content-length");
      response.add("content-encoding", "gzip");
      response.chunked = true;
      response.body_view(ctx.arena().copy(packed));
    }
    co_return finish(std::move(response));
  }
  net::Response response = ctx.response(404);
  response.add("content-type", "text/html; charset=UTF-8");
  if (wants_gzip(request)) {
    response.add("content-encoding", "gzip");
    response.framed = true;  // the Rust port sends this body in two chunks
    response.body_view(data::f_404_html_gz_chunked);
  } else {
    response.add("content-length", "4237");
    response.body_view(data::f_404_html);
  }
  co_return finish(std::move(response));
}

}  // namespace campfire::app
