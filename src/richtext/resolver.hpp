// An AttachableResolver that checks SGIDs with src/compat and finds records through a small
// interface. Rails: GlobalID::Locator, SignedGlobalID. Rust: crates/rails_compat/src/global_id.rs
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "compat/secrets.hpp"
#include "compat/time.hpp"
#include "richtext/attachables.hpp"

namespace campfire::richtext {

// The records that rendering needs. The app fills it from the database.
class RecordLookup {
 public:
  RecordLookup() = default;
  RecordLookup(const RecordLookup&) = delete;
  RecordLookup& operator=(const RecordLookup&) = delete;
  virtual ~RecordLookup() = default;

  [[nodiscard]] virtual std::optional<MentionUser> user(std::int64_t id) const = 0;
  // Does a record of another model exist (Room, Message and so on)?
  [[nodiscard]] virtual bool record_exists(std::string_view model, std::int64_t id) const = 0;
};

class CompatResolver final : public AttachableResolver {
 public:
  CompatResolver(const compat::Secrets& secrets, compat::Timestamp now, const RecordLookup& records)
      : secrets_(secrets), now_(now), records_(records) {}

  [[nodiscard]] SignedLookup locate_signed(std::string_view sgid) const override;
  [[nodiscard]] GidLookup find_gid(const compat::global_id::GlobalId& gid) const override;

 private:
  const compat::Secrets& secrets_;
  compat::Timestamp now_;
  const RecordLookup& records_;
};

}  // namespace campfire::richtext
