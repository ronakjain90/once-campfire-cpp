// Rails: app/controllers/messages_controller.rb, app/controllers/concerns/room_scoped.rb, app/models/message/*.rb.
// Rust: crates/campfire/src/controllers/messages.rs.
#include "app/message_actions.hpp"

#include <algorithm>

#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/message_partial.hpp"
#include "app/page.hpp"
#include "assets/assets.hpp"
#include "compat/ruby.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "models/job_sink.hpp"
#include "models/storage_records.hpp"
#include "richtext/content.hpp"
#include "richtext/richtext.hpp"
#include "richtext/text_util.hpp"
#include "routes/routes.hpp"
#include "storage/storage.hpp"
#include "views/layout.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::messages {

namespace {

constexpr std::string_view kInvalidAttachment = "Could not find or build blob: expected attachable";

std::string render_string(const std::function<void(Out&)>& render) {
  Out out;
  render(out);
  return out.to_string();
}

}  // namespace

// `ActionText::Content.new(body, canonicalize: true).to_html`: assigning a String to a rich text attribute stores it.
std::string canonical_body(MessagePresenter& presenter, std::string_view body) {
  auto content = richtext::Content::load(body, presenter.render_context());
  return content ? content->to_html() : std::string(body);
}

// `plain_text_body`: `body.to_plain_text.presence || attachment&.filename&.to_s || ""`
std::string plain_text_for(MessagePresenter& presenter, std::string_view body, std::string_view filename) {
  if (!body.empty()) {
    auto text = richtext::to_plain_text(body, presenter.render_context());
    if (text && !richtext::is_blank(*text)) return std::move(*text);
  }
  return std::string(filename);
}

std::unexpected<Failure> db_failure(const Error& error) {
  if (error.code == Errc::NotFound) return fail_with(ErrorKind::NotFound, error.message);
  return fail_internal(error.message);
}

AttachmentParam attachment_param(const req::ParamMap& permitted) {
  AttachmentParam out;
  const req::Param* param = permitted.get("attachment");
  if (param == nullptr) return out;  // the key was not given
  if (param->is_null() || param->as_str() == std::optional<std::string_view>("")) {
    out.kind = AttachmentParam::Kind::Delete;
    return out;
  }
  if (const auto* file = param->as_file()) {
    out.kind = AttachmentParam::Kind::Upload;
    out.file = *file;
    return out;
  }
  out.kind = AttachmentParam::Kind::Invalid;
  return out;
}

Flow<MessageParams> message_params(Rq& rq) {
  auto required = rq.params().require("message");
  if (!required) return fail_with(ErrorKind::ParameterMissing, required.error().message);
  MessageParams out;
  const req::ParamMap* hash = (*required)->as_hash();
  if (hash == nullptr) return out;  // anything that is not a hash permits nothing
  const req::ParamMap permitted = hash->permit({"body", "attachment", "client_message_id"}, rq.ctx.resource());
  const auto text = [&](std::string_view key) -> std::optional<std::string> {
    const req::Param* param = permitted.get(key);
    if (param == nullptr) return std::nullopt;
    if (const auto s = param->as_str()) return std::string(*s);
    return std::nullopt;
  };
  out.body = text("body");
  out.client_message_id = text("client_message_id");
  out.attachment = attachment_param(permitted);
  return out;
}

Flow<models::RoomRef> set_room(Rq& rq) {
  const models::User* user = rq.current_user();
  if (user == nullptr) return fail_with(ErrorKind::NotFound);
  const auto room_id = rq.param_str("room_id").and_then(compat::integer_cast);
  if (!room_id) return fail_with(ErrorKind::NotFound, "Couldn't find Membership");
  auto room = models::room_refs::find_for_user(rq.db(), rq.arena(), user->id, *room_id);
  if (!room) return db_failure(room.error());
  if (!*room) return fail_with(ErrorKind::NotFound, "Couldn't find Membership");
  return std::move(**room);
}

Flow<models::Message> set_message(Rq& rq, const models::RoomRef& room) {
  const auto id = rq.param_str("id").and_then(compat::integer_cast);
  if (!id) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  auto message = models::messages::find_in_room(rq.db(), rq.arena(), room.id, *id);
  if (!message) return db_failure(message.error());
  if (!*message) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  return std::move(**message);
}

Flow<void> ensure_can_administer(Rq& rq, const models::Message& message) {
  const models::User* user = rq.current_user();
  // `administrator? || self == record&.creator`
  if (user == nullptr || !(user->is_administrator() || user->id == message.creator_id)) {
    return halt(concerns::head_in_before_action(rq, 403));
  }
  return {};
}

Flow<net::Response> content_page(Rq& rq, int status, bool always_application,
                                 const std::function<void(Out&, const views::ViewContext&)>& content) {
  auto layout = load_layout(rq);
  if (!layout) return std::unexpected(std::move(layout.error()));
  const views::ViewContext ctx = make_view_context(rq, *layout);
  views::LayoutParts parts;
  parts.content = [&](Out& o) { content(o, ctx); };
  Out out(rq.ctx.resource());
  if (rq.is_turbo_frame_request() && !always_application) {
    views::layouts::turbo_rails::frame(out, parts);
    return rq.html(status, std::move(out));
  }
  views::layouts::application(out, ctx, parts);
  return layout_response(rq, status, std::move(out));
}

Flow<views::ViewContext> detached_context(Rq& rq) {
  // The detached view reads the account only: it needs no user and no last room.
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) return fail_internal(account.error().message);
  views::ViewContext ctx;
  if (*account) {
    const models::Account& a = **account;
    ctx.account.name = a.name;
    ctx.account.logo_url = campfire::routes::fresh_account_logo(parse_db(a.updated_at) ? format_to_fs_number(*parse_db(a.updated_at)) : std::string{});
    ctx.account.has_logo = a.has_logo;
  } else {
    ctx.account.logo_url = campfire::routes::fresh_account_logo();
  }
  ctx.vapid_public_key = rq.app.config.vapid_public_key;
  ctx.asset_path = [](std::string_view source) {
    auto path = assets::asset_path(source);
    if (!path) throw std::runtime_error(path.error().message);
    return std::move(*path);
  };
  ctx.importmap_tags = std::string(assets::javascript_importmap_tags());
  ctx.stylesheet_tags = rq.app.stylesheets.html;
  // `SetCurrentRequest`: `default_url_options` carry the host and the protocol only, so the port never shows.
  ctx.base_url = std::string(rq.info.protocol()) + rq.info.host();
  ctx.request_url = ctx.base_url + "/";
  ctx.app_version = rq.app.config.app_version;
  return ctx;
}

namespace {

struct StagedUpload {
  std::optional<storage::Staged> staged;
  std::string filename;  // `blob.filename.to_s`, for the search index
};

// The file half of `Blob.create_and_upload!`: copies, checksums and identifies the file, off the worker and the writer.
Task<Flow<StagedUpload>> stage_upload(Rq& rq, const AttachmentParam& attachment) {
  StagedUpload out;
  if (attachment.kind == AttachmentParam::Kind::Invalid) co_return fail_internal(std::string(kInvalidAttachment));
  if (attachment.kind != AttachmentParam::Kind::Upload) co_return out;
  const req::UploadedFile& file = *attachment.file;
  const storage::Storage& store = *rq.app.storage;
  auto staged = co_await rq.ctx.offload(rq.app.jobs, [&]() -> Result<storage::Staged> {
    return store.stage_file(file.path, storage::Filename::from_bytes(file.original_filename),
                            file.content_type ? std::optional<std::string_view>(*file.content_type) : std::nullopt);
  });
  if (!staged) co_return fail_internal(staged.error().message);
  out.filename = staged->blob().filename.sanitized();
  out.staged.emplace(std::move(*staged));
  co_return out;
}

// What the media work needs from a request or from a job: the app, a scheduler to resume on, and a reader.
struct MediaWork {
  const App& app;
  Scheduler& scheduler;
  db::Connection& conn;
};

// `Ctx::offload` for work that has no request.
template <class F>
auto offload(MediaWork& work, F fn) {
  using R = std::invoke_result_t<F&>;
  auto pair = make_completion<R>(work.scheduler);
  work.app.jobs.submit([fn = std::move(fn), setter = pair.second]() mutable {
    try {
      setter.set_value(fn());
    } catch (...) {
      setter.set_exception(std::current_exception());
    }
  });
  return std::move(pair.first);
}

// `Blob#analyze`, then `touch_attachments`: the message is touched, and so is its room.
Task<Flow<void>> analyze_attachment(MediaWork& work, const storage::Blob& blob, models::Message& message,
                                    std::string_view plain_text) {
  const storage::Storage& store = *work.app.storage;
  auto metadata = co_await offload(work, [&] { return store.analyzed_metadata(blob); });
  if (!metadata) co_return fail_internal(metadata.error().message);
  auto written = co_await work.app.db->write(work.scheduler, [&](db::Tx& tx) -> Status {
    models::DbRecords records(tx.conn());
    if (auto s = records.update_metadata(blob.id, *metadata); !s) return s;
    return models::messages::touch(tx, message, plain_text);
  });
  if (!written) co_return db_failure(written.error());
  co_return Flow<void>{};
}

// `blob.representation(variation).processed` for a blob that is not a video: the variant is made off the writer, then
// recorded.
Task<Flow<void>> process_variant(MediaWork& work, const storage::Blob& blob, const compat::Variation& variation) {
  const storage::Storage& store = *work.app.storage;
  {
    models::DbRecords records(work.conn);
    auto existing = store.existing_variant(records, blob, variation);
    if (!existing) co_return fail_internal(existing.error().message);
    if (*existing) co_return Flow<void>{};
  }
  auto image = co_await offload(work, [&] { return store.transform_variant(blob, variation); });
  if (!image) co_return fail_internal(image.error().message);
  auto recorded = co_await work.app.db->write(work.scheduler, [&](db::Tx& tx) -> Status {
    models::DbRecords records(tx.conn());
    auto saved = store.record_variant(records, blob, variation, *image, models::to_compat(tx.now()));
    if (!saved) return std::unexpected(saved.error());
    if (*saved) tx.after_commit([&image] { image->keep(); });
    return {};
  });
  if (!recorded) co_return db_failure(recorded.error());
  co_return Flow<void>{};
}

// `blob.preview_image`: the frame that ffmpeg draws, attached to the blob as `preview_image`.
Task<Flow<storage::Blob>> preview_image(MediaWork& work, const storage::Blob& blob) {
  const storage::Storage& store = *work.app.storage;
  {
    models::DbRecords records(work.conn);
    auto existing = store.existing_preview_image(records, blob);
    if (!existing) co_return fail_internal(existing.error().message);
    if (*existing) co_return std::move(**existing);
  }
  auto image = co_await offload(work, [&] { return store.draw_preview_image(blob); });
  if (!image) co_return fail_internal(image.error().message);
  std::optional<storage::Blob> result;
  auto recorded = co_await work.app.db->write(work.scheduler, [&](db::Tx& tx) -> Status {
    models::DbRecords records(tx.conn());
    auto saved = store.record_preview_image(records, blob, *image, models::to_compat(tx.now()));
    if (!saved) return std::unexpected(saved.error());
    if (*saved) {
      tx.after_commit([&image] { image->keep(); });
      result = std::move(*saved);
      return {};
    }
    // Another request drew it first: ours is dropped, and its file is deleted.
    auto winner = store.existing_preview_image(records, blob);
    if (!winner) return std::unexpected(winner.error());
    result = std::move(*winner);
    return {};
  });
  if (!recorded) co_return db_failure(recorded.error());
  if (!result) co_return fail_internal("ActiveStorage::Blob preview image is missing");
  co_return std::move(*result);
}

// `process_attachment`: `ensure_attachment_analyzed`, then `process_attachment_thumbnail`.
Task<Flow<void>> process_media(MediaWork& work, storage::Blob blob, models::Message& message,
                               std::string_view plain_text) {
  const storage::Storage& store = *work.app.storage;
  if (auto analyzed = co_await analyze_attachment(work, blob, message, plain_text); !analyzed) {
    co_return std::unexpected(std::move(analyzed.error()));
  }
  using compat::marshal::Value;
  if (blob.is_video()) {
    // `attachment.preview(format: :webp).processed`
    auto image = co_await preview_image(work, blob);
    if (!image) co_return std::unexpected(std::move(image.error()));
    const compat::Variation webp({{"format", Value::symbol("webp")}});
    auto variation = store.variation_for(*image, webp);
    if (!variation) co_return fail_internal(variation.error().message);
    co_return co_await process_variant(work, *image, *variation);
  }
  if (blob.is_representable()) {
    // `attachment.representation(:thumb).processed`
    const compat::Variation thumb = compat::Variation::resize_to_limit(1200, 800, std::nullopt);
    auto variation = store.variation_for(blob, thumb);
    if (!variation) co_return fail_internal(variation.error().message);
    co_return co_await process_variant(work, blob, *variation);
  }
  co_return Flow<void>{};
}

}  // namespace

Task<Status> process_attachment_detached(const App& app, Scheduler& scheduler, db::Connection& conn, storage::Blob blob,
                                         models::Message& message, std::string_view plain_text) {
  MediaWork work{app, scheduler, conn};
  auto done = co_await process_media(work, std::move(blob), message, plain_text);
  if (done) co_return Status{};
  if (const auto* error = std::get_if<HttpError>(&done.error())) co_return fail(Errc::Internal, error->message);
  co_return fail(Errc::Internal, "processing the attachment failed");
}

Task<Flow<models::Message>> create_message(Rq& rq, const models::RoomRef& room, MessageParams params) {
  const models::User* user = rq.current_user();
  if (user == nullptr) co_return fail_with(ErrorKind::NotFound);
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto upload = co_await stage_upload(rq, params.attachment);
  if (!upload) co_return std::unexpected(std::move(upload.error()));

  models::NewMessage attributes;
  attributes.room_id = room.id;
  attributes.creator_id = user->id;
  attributes.client_message_id = params.client_message_id;
  std::string body_text;
  if (params.body) {
    body_text = canonical_body(presenter, *params.body);
    attributes.body = body_text;
  }
  attributes.plain_text = plain_text_for(presenter, body_text, upload->filename);

  std::optional<storage::Blob> blob;
  models::JobSink* sink = rq.app.job_sink.get();
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Result<models::Message> {
    if (upload->staged) {
      models::DbRecords records(tx.conn());
      auto saved = upload->staged->insert(records, models::to_compat(tx.now()));
      if (!saved) return std::unexpected(saved.error());
      blob = std::move(*saved);
      attributes.attachment_blob_id = blob->id;
      tx.after_commit([&upload] { upload->staged->keep(); });
    }
    return models::messages::create(tx, attributes, sink);
  });
  if (!written) co_return db_failure(written.error());
  models::Message message = std::move(*written);
  if (blob) {
    MediaWork work{rq.app, rq.ctx.scheduler(), rq.db()};
    if (auto processed = co_await process_media(work, *blob, message, attributes.plain_text); !processed) {
      co_return std::unexpected(std::move(processed.error()));
    }
  }
  co_return message;
}

Task<Flow<models::Message>> update_message(Rq& rq, models::Message message, MessageParams params) {
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto upload = co_await stage_upload(rq, params.attachment);
  if (!upload) co_return std::unexpected(std::move(upload.error()));
  const bool attachment_given = params.attachment.kind != AttachmentParam::Kind::Unchanged;

  // The plain text of the state after the update.
  std::string body_text;
  if (params.body) {
    body_text = canonical_body(presenter, *params.body);
  } else if (auto current = presenter.body_html(message); current) {
    body_text = std::move(*current);
  }
  std::string filename = upload->filename;
  if (!attachment_given) {
    auto attached = presenter.attachment(message);
    if (attached && *attached) filename = (*attached)->filename;
  }
  const std::string plain_text = plain_text_for(presenter, body_text, filename);

  std::optional<storage::Blob> blob;
  models::messages::ReplacedAttachment replaced;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    if (params.body) {
      if (auto s = models::messages::update_body(tx, message, body_text, plain_text); !s) return s;
    }
    if (attachment_given) {
      std::optional<std::int64_t> blob_id;
      if (upload->staged) {
        models::DbRecords records(tx.conn());
        auto saved = upload->staged->insert(records, models::to_compat(tx.now()));
        if (!saved) return std::unexpected(saved.error());
        blob = std::move(*saved);
        blob_id = blob->id;
        tx.after_commit([&upload] { upload->staged->keep(); });
      }
      auto result = models::messages::replace_attachment(tx, message, blob_id, plain_text);
      if (!result) return std::unexpected(result.error());
      replaced = *result;
    }
    return {};
  });
  if (!written) co_return db_failure(written.error());
  // `has_one_attached` replaces the attachment and `purge_later` queues the old blob.
  if (replaced.purged_blob_id) rq.app.job_sink->purge_blob(*replaced.purged_blob_id);
  if (blob) {
    // `ActiveStorage::AnalyzeJob`: the new blob is analyzed, not processed (verified against the reference).
    MediaWork work{rq.app, rq.ctx.scheduler(), rq.db()};
    if (auto analyzed = co_await analyze_attachment(work, *blob, message, plain_text); !analyzed) {
      co_return std::unexpected(std::move(analyzed.error()));
    }
  }
  auto reloaded = models::messages::find_by_id(rq.db(), rq.arena(), message.id);
  if (!reloaded) co_return db_failure(reloaded.error());
  if (!*reloaded) co_return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  co_return std::move(**reloaded);
}

Task<Flow<void>> destroy_message(Rq& rq, const models::RoomRef& room, const models::Message& message) {
  models::messages::ReplacedAttachment destroyed;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    auto result = models::messages::destroy(tx, message);
    if (!result) return std::unexpected(result.error());
    destroyed = *result;
    return {};
  });
  if (!written) co_return db_failure(written.error());
  // `dependent: :purge_later`
  if (destroyed.purged_blob_id) rq.app.job_sink->purge_blob(*destroyed.purged_blob_id);
  broadcasts::message_remove(rq.app, room, message.client_message_id);
  co_return Flow<void>{};
}

Result<std::string> broadcast_create_detached(const App& app, db::Connection& conn, Arena& arena,
                                              const models::RoomRef& room, const models::Message& message) {
  // `ApplicationController.renderer`: no request, so the host is `example.org`.
  views::ViewContext ctx;
  auto account = models::accounts::first(conn, arena);
  if (!account) return std::unexpected(account.error());
  const auto fs_number = [](const std::string& text) {
    const auto t = parse_db(text);
    return t ? format_to_fs_number(*t) : std::string{};
  };
  if (*account) {
    ctx.account.name = (*account)->name;
    ctx.account.logo_url = campfire::routes::fresh_account_logo(fs_number((*account)->updated_at));
    ctx.account.has_logo = (*account)->has_logo;
  } else {
    ctx.account.logo_url = campfire::routes::fresh_account_logo();
  }
  ctx.vapid_public_key = app.config.vapid_public_key;
  ctx.asset_path = [](std::string_view source) {
    auto path = assets::asset_path(source);
    if (!path) throw std::runtime_error(path.error().message);
    return std::move(*path);
  };
  ctx.importmap_tags = std::string(assets::javascript_importmap_tags());
  ctx.stylesheet_tags = app.stylesheets.html;
  ctx.base_url = "http://example.org";
  ctx.request_url = ctx.base_url + "/";
  ctx.app_version = app.config.app_version;

  MessagePresenter presenter(conn, arena, app, std::string());
  auto view = presenter.message(message);
  if (!view) return std::unexpected(view.error());
  std::string html = render_string([&](Out& out) { views::messages::message(out, ctx, *view); });
  broadcasts::message_append(app, room, html);
  auto members = models::room_refs::member_user_ids(conn, arena, room.id);
  if (!members) return std::unexpected(members.error());
  broadcasts::unread_room(app, room, *members);
  return html;
}

Flow<std::string> broadcast_create(Rq& rq, const models::RoomRef& room, const models::Message& message) {
  auto ctx = detached_context(rq);
  if (!ctx) return std::unexpected(std::move(ctx.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.message(message);
  if (!view) return db_failure(view.error());
  std::string html = render_string([&](Out& out) { views::messages::message(out, *ctx, *view); });
  broadcasts::message_append(rq.app, room, html);
  auto members = models::room_refs::member_user_ids(rq.db(), rq.arena(), room.id);
  if (!members) return db_failure(members.error());
  broadcasts::unread_room(rq.app, room, *members);
  return html;
}

Flow<void> broadcast_replace(Rq& rq, const models::RoomRef& room, const models::Message& message) {
  auto ctx = detached_context(rq);
  if (!ctx) return std::unexpected(std::move(ctx.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.message(message);
  if (!view) return db_failure(view.error());
  const std::string html = render_string([&](Out& out) { views::messages::presentation(out, *ctx, *view); });
  broadcasts::message_replace_presentation(rq.app, room, message.client_message_id, html);
  return {};
}

Flow<void> deliver_webhooks_to_bots(Rq& rq, const models::RoomRef& room, const models::Message& message) {
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  std::vector<models::User> candidates;
  if (room.direct()) {
    auto bots = models::room_refs::active_bots(rq.db(), rq.arena(), room.id);
    if (!bots) return db_failure(bots.error());
    candidates = std::move(*bots);
  } else {
    // `@message.mentionees`: the mentioned users who are members of the room.
    auto body = presenter.body_html(message);
    if (!body) return db_failure(body.error());
    auto mentioned = richtext::mentioned_users(*body, presenter.render_context());
    if (!mentioned) return fail_internal(mentioned.error().message);
    std::vector<std::int64_t> ids;
    for (const richtext::MentionUser& user : *mentioned) ids.push_back(user.id);
    auto members = models::room_refs::members_among(rq.db(), rq.arena(), room.id, ids);
    if (!members) return db_failure(members.error());
    candidates = std::move(*members);
  }
  models::JobSink* sink = rq.app.job_sink.get();
  for (const models::User& user : candidates) {
    // `.active_bots.excluding(@message.creator)`
    if (!user.is_bot() || user.status != models::kStatusActive || user.id == message.creator_id) continue;
    sink->deliver_webhook(user.id, message.id);
  }
  return {};
}

}  // namespace campfire::app::messages
