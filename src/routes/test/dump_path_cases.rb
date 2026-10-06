# Writes path_cases.json: what the Rails `*_path` helpers return for tricky arguments, and what the
# two `direct` routes build on (`account_logo_path`, `user_avatar_path` with a `v` query).
# Run in the campfire-reference image:
#   docker run --rm -i --entrypoint "" -e RAILS_ENV=production -e SECRET_KEY_BASE=x -e DISABLE_SSL=1 \
#     -e DATABASE_URL=sqlite3:/tmp/r.sqlite3 -v $PWD/src/routes/test:/w campfire-reference:app \
#     bin/rails runner /w/dump_path_cases.rb /w
require "json"

helpers = Rails.application.routes.url_helpers
named = JSON.parse(File.read(File.join(ARGV.fetch(0), "named_routes.json")))
values = ["1", "22", "a b", "é/ü", "x?y#z", "1-abc", "%41", "a+b", "@:;=", "Aa-._~!$&'()*,", "ab ", "\u{1F600}", "[x]", "a\"b<c>"]

cases = []
named.each do |route|
  name = route["name"]
  route_obj = Rails.application.routes.named_routes[name]
  count = route_obj.required_parts.size
  values.each_with_index do |_, i|
    args = Array.new(count) { |j| values[(i + j * 3) % values.size] }
    cases << { "name" => name, "args" => args, "path" => helpers.public_send("#{name}_path", *args) }
  end
end

queries = []
[[nil, nil], ["20260101120000", nil], [nil, "128x128"], ["20260101120000", "64"]].each do |v, size|
  options = { v: v, size: size }.compact
  queries << { "v" => v, "size" => size, "path" => helpers.account_logo_path(**options) }
end
avatars = ["tok-en==", "a b"].map do |token|
  { "token" => token, "v" => "20260101120000", "path" => helpers.user_avatar_path(token, v: "20260101120000") }
end
directs = { "rooms_directs_with_users" => helpers.rooms_directs_path(user_ids: [5, 6, 70]) }
File.write(File.join(ARGV.fetch(0), "path_cases.json"), JSON.pretty_generate({ "cases" => cases, "logo" => queries, "avatars" => avatars, "directs" => directs }) + "\n")
