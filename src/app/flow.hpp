// The result of an action and of a before action. Rails: a callback that renders or redirects halts
// the chain; an exception gives the status of ActionDispatch::ExceptionWrapper.rescue_responses.
// Rust: crates/kit/src/error.rs (Error, halt).
#pragma once

#include <expected>
#include <string>
#include <utility>
#include <variant>

#include "net/response.hpp"

namespace campfire::app {

enum class ErrorKind {
  BadRequest,                 // ActionController::BadRequest, parameter errors: 400
  ParameterMissing,           // 400
  InvalidAuthenticityToken,   // 422
  InvalidCrossOriginRequest,  // 422
  UnknownFormat,              // 406
  NotFound,                   // 404
  MethodNotAllowed,           // 405
  CookieOverflow,             // 500
  UnsafeRedirect,             // 500
  IpSpoofAttack,              // 500
  Status,                     // the status in the error
  Internal,                   // 500
};

struct HttpError {
  ErrorKind kind = ErrorKind::Internal;
  int status_override = 0;  // for `Status`
  std::string message;

  [[nodiscard]] int status() const noexcept {
    switch (kind) {
      case ErrorKind::BadRequest:
      case ErrorKind::ParameterMissing: return 400;
      case ErrorKind::InvalidAuthenticityToken:
      case ErrorKind::InvalidCrossOriginRequest: return 422;
      case ErrorKind::UnknownFormat: return 406;
      case ErrorKind::NotFound: return 404;
      case ErrorKind::MethodNotAllowed: return 405;
      case ErrorKind::Status: return status_override;
      case ErrorKind::CookieOverflow:
      case ErrorKind::UnsafeRedirect:
      case ErrorKind::IpSpoofAttack:
      case ErrorKind::Internal: return 500;
    }
    return 500;
  }
};

// A before action (or an action) that made the response itself.
struct Halt {
  net::Response response;
};

using Failure = std::variant<Halt, HttpError>;
template <class T = net::Response>
using Flow = std::expected<T, Failure>;

[[nodiscard]] inline std::unexpected<Failure> halt(net::Response response) {
  return std::unexpected<Failure>(Failure(std::in_place_type<Halt>, Halt{std::move(response)}));
}
[[nodiscard]] inline std::unexpected<Failure> fail_with(ErrorKind kind, std::string message = {}) {
  return std::unexpected<Failure>(Failure(std::in_place_type<HttpError>, HttpError{kind, 0, std::move(message)}));
}
[[nodiscard]] inline std::unexpected<Failure> fail_status(int status, std::string message = {}) {
  return std::unexpected<Failure>(
      Failure(std::in_place_type<HttpError>, HttpError{ErrorKind::Status, status, std::move(message)}));
}
[[nodiscard]] inline std::unexpected<Failure> fail_internal(std::string message) {
  return fail_with(ErrorKind::Internal, std::move(message));
}

}  // namespace campfire::app
