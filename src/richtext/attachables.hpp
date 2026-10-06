// Action Text attachables: resolving <action-text-attachment> nodes and rendering their partials.
// Rails: actiontext attachment.rb, lexxy attachables, reference/lib/rails_ext/action_text_attachables.rb,
// reference/lib/rails_ext/actiontext_opengraph_embeds.rb. Rust: crates/richtext/src/attachables.rs
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "compat/global_id.hpp"
#include "richtext/dom.hpp"
#include "richtext/error.hpp"

namespace campfire::richtext {

inline constexpr std::string_view kMentionContentType = "application/vnd.campfire.mention";
inline constexpr std::string_view kOpengraphEmbedContentType = "application/vnd.actiontext.opengraph-embed";

// What the app knows about a user, for rendering users/_mention.html.erb. Plain text and mentions
// read `id` and `name`. The other fields are only rendered.
struct MentionUser {
  std::int64_t id = 0;
  std::string name;
  std::string title;           // User#title: name and bio joined with " – "
  std::string attachable_sgid;  // user.attachable_sgid: a new SGID for the "attachable" purpose
  std::string user_path;        // user_path(user)
  std::string avatar_path;      // fresh_user_avatar_path(user)
};

// What a signed GlobalID for the "attachable" purpose points at.
struct SignedLookup {
  enum class Kind : std::uint8_t {
    User,           // The signature verified and the user exists.
    MissingRecord,  // The signature verified, but the record is gone.
    Invalid,        // Bad signature, wrong purpose, expired, or not an SGID.
  };
  Kind kind = Kind::Invalid;
  MentionUser user;         // Kind::User
  std::string model_name;   // Kind::MissingRecord
};

// A record that `GlobalID.find` located.
struct GidLookup {
  enum class Kind : std::uint8_t {
    User,
    OtherModel,  // Found, but not a User: the fallback for an invalid signature ignores it.
    NotFound,    // nil, or ActiveRecord::RecordNotFound
    Raises,      // Any other exception (an unknown model constant, say)
  };
  Kind kind = Kind::NotFound;
  MentionUser user;
};

// The access of the app to its records. The tests fill it from the corpus. The app fills it from
// the database. `CompatResolver` (resolver.hpp) checks the SGID with `src/compat`.
class AttachableResolver {
 public:
  AttachableResolver() = default;
  AttachableResolver(const AttachableResolver&) = delete;
  AttachableResolver& operator=(const AttachableResolver&) = delete;
  virtual ~AttachableResolver() = default;

  // `GlobalID::Locator.locate_signed(sgid, for: "attachable")`. When that finds nothing: does
  // `SignedGlobalID.parse(sgid, for: "attachable")` still verify?
  [[nodiscard]] virtual SignedLookup locate_signed(std::string_view sgid) const = 0;
  // `GlobalID.find(gid)`, with no signature.
  [[nodiscard]] virtual GidLookup find_gid(const compat::global_id::GlobalId& gid) const = 0;
};

// All that rendering needs from the request and the app.
struct RenderContext {
  const AttachableResolver& resolver;
  std::string request_host;  // Current.request_host. Empty when unset.
};

struct OpengraphEmbed {
  std::optional<std::string> href;
  std::optional<std::string> url;
  std::optional<std::string> filename;
  std::optional<std::string> description;

  [[nodiscard]] bool twitter_avatar() const;
};

struct Attachable {
  enum class Kind : std::uint8_t {
    User,
    OpengraphEmbed,
    Content,      // ActionText::Attachables::ContentAttachment
    RemoteImage,  // ActionText::Attachables::RemoteImage
    RemoteVideo,  // Lexxy ActionText::Attachables::RemoteVideo
    Missing,      // ActionText::Attachables::MissingAttachable
  };
  Kind kind = Kind::Missing;
  MentionUser user;
  richtext::OpengraphEmbed embed;
  std::string content;       // Content
  std::string url;           // RemoteImage, RemoteVideo
  std::string content_type;  // RemoteVideo
  std::optional<std::string> width;
  std::optional<std::string> height;
  std::optional<std::string> filename;  // RemoteVideo
};

// The attachment node's caption, and the attachable that the node names.
struct Attachment {
  Attachable attachable;
  std::optional<std::string> caption;
};

// Campfire's `ActionText::Attachment.from_node`: an opengraph embed, else a User found through a
// SGID that may be invalid, else the lookup of Action Text (as Lexxy extends it).
[[nodiscard]] Result<Attachment> attachment_from_node(const Node* node, const RenderContext& ctx);

// `ActionText::Attachable.from_node`, with the RemoteVideo fallback of Lexxy. `Content#attachables`
// (and so `Message#mentionees`) uses this, without the fallback of Campfire for an invalid SGID.
[[nodiscard]] Attachable action_text_attachable_from_node(const Node* node, const RenderContext& ctx);

// `attachable_content_type`, which only some attachables have.
[[nodiscard]] Result<std::string_view> attachable_content_type(const Attachable& attachable);

// `ActionText::Attachment::OpengraphEmbed.from_node`
[[nodiscard]] Result<std::optional<OpengraphEmbed>> opengraph_embed_from_node(const Node* node,
                                                                              const RenderContext& ctx);

// `web_url`: an absolute http(s) URL on a named host other than the host of this Campfire.
[[nodiscard]] Result<std::optional<std::string>> web_url(std::optional<std::string_view> value,
                                                         std::string_view request_host);

// `render_action_text_attachment(attachment)`: the partial of the attachable, chomped.
// `render_content` renders the content of a nested content attachment (ContentAttachment#to_html).
using RenderContentFn = std::function<Result<std::string>(std::string_view content)>;
[[nodiscard]] Result<std::string> render_attachment(const Attachment& attachment, const RenderContentFn& render_content);

// reference/app/views/users/_mention.html.erb, with `avatar_tag`.
[[nodiscard]] std::string render_mention(const MentionUser& user);
// reference/app/views/action_text/attachables/_opengraph_embed.html.erb
[[nodiscard]] std::string render_opengraph_embed(const OpengraphEmbed& embed);

// `Attachment#to_plain_text`. A string replaces the node after it is parsed as markup in the
// parent of the node. Content is a fragment that is moved in as it is.
struct PlainTextRepresentation {
  bool is_content = false;
  std::string text;
};
[[nodiscard]] PlainTextRepresentation attachment_plain_text(const Attachment& attachment);

}  // namespace campfire::richtext
