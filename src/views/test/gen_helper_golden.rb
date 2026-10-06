# Writes helper_golden.json: what the Rails helpers write for fixed calls. helpers_test.cpp makes the
# same calls with the C++ helpers and compares the bytes. Run in the campfire-reference image:
#   docker run --rm -i --entrypoint "" --user "$(id -u):$(id -g)" -e RAILS_ENV=production \
#     -e SECRET_KEY_BASE=x -e DISABLE_SSL=1 -e DATABASE_URL=sqlite3:/tmp/r.sqlite3 \
#     -v $PWD/src/views/test:/w campfire-reference:app bin/rails runner /w/gen_helper_golden.rb /w
require "json"

ActionController::Base.allow_forgery_protection = false
v = ApplicationController.new.view_context
safe = ->(s) { s.html_safe }
cases = {}
add = ->(name, html) { cases[name] = html.to_str }

add.("tag_div", v.tag.div("x<", class: "a&b", hidden: true, data: { turbo_frame: "_top", x: "<" }, aria: { hidden: "true" }, tabindex: -1))
add.("tag_br", v.tag.br(class: "c"))
add.("tag_turbo_frame_name", v.tag.turbo_frame(id: "f"))
add.("content_tag_textarea", v.content_tag(:textarea, "x<", id: "t"))
add.("attrs_boolean", v.tag.input(type: "checkbox", checked: false, disabled: true, required: false, "data-x": false, "aria-y": true))
add.("attrs_safe_value", v.tag.div(data: { action: safe.("a->\"b\"&c") }))
add.("legacy_tag", v.tag(:img, alt: "a", src: "/x.png"))
add.("link_to", v.link_to("a<b", "/x?y=1&z=2", class: "btn", data: { turbo: false }))
add.("link_to_block", v.link_to("/x", class: "btn", id: "i") { safe.("<b>c</b>") })
add.("link_to_if_false", v.link_to_if(false, "a<", "/x"))
add.("link_to_if_true", v.link_to_if(true, "a<", "/x", title: "t"))
add.("mail_to", v.mail_to("a+b@x.com"))
add.("button_to_delete", v.button_to("/x?a=1&b=2", method: :delete, class: "btn", form_class: "f") { safe.("go") })
add.("button_to_default", v.button_to("/x", class: "btn", data: { turbo: false }) { safe.("go<") })
add.("button_to_put_and_get", v.button_to("/x", method: :put) { safe.("a") } + v.button_to("/y", method: :get) { safe.("b") } + v.button_to("/z", method: :patch, title: "p") { safe.("c") })
add.("image_tag_size", v.image_tag("/img.svg", aria: { hidden: true }, size: 20))
add.("image_tag_size_xy", v.image_tag("/img.svg", alt: "A<", size: "20x30", class: "c"))
add.("image_tag_url", v.image_tag("https://example.com/a.png", class: "c"))
add.("turbo_frame_tag", v.turbo_frame_tag("f", src: "/x", target: "_top", class: "c") { safe.("in") })
add.("turbo_frame_tag_plain", v.turbo_frame_tag("room_1", class: "c") { safe.("in") })
add.("turbo_stream_append", v.turbo_stream.append("messages", safe.("<p>x</p>")))
add.("turbo_stream_replace", v.turbo_stream.replace("message_1", safe.("<p>y</p>")))
add.("turbo_stream_remove", v.turbo_stream.remove("message_1"))
add.("local_datetime_tag", v.local_datetime_tag(Time.utc(2026, 1, 1, 12, 30, 5), style: :date, class: "x"))
add.("local_datetime_tag_time", v.local_datetime_tag(Time.utc(2026, 6, 1, 0, 0, 0)))
add.("page_requires_reload", v.turbo_page_requires_reload_tag)
signed = Turbo::StreamsChannel.signed_stream_name("room_1")
add.("turbo_stream_from", v.turbo_stream_from("room_1"))

form = v.form_with(url: "/x?a=1", scope: :user, class: "c", id: "i", method: :patch, data: { controller: "form", action: "a->b" }) do |f|
  f.text_field(:name, value: "a<b", class: "input", maxlength: 5) +
    f.email_field(:email, value: nil, required: true) +
    f.password_field(:password, value: "ignored", autocomplete: "off") +
    f.hidden_field(:h, value: "1") +
    f.text_area(:bio, value: "x\n<y", rows: 3) +
    f.check_box(:ok, { class: "c", checked: true }, "1", "0") +
    f.check_box(:off, {}, "1", "0") +
    f.file_field(:avatar, accept: "image/*") +
    f.url_field(:site, value: "http://x", name: "custom", id: "cid") +
    f.button(class: "btn") { safe.("Save") }
end
add.("form_with_fields", form)
add.("form_with_plain", v.form_with(url: "/search", method: :get) { |f| f.text_field(:q, value: "v", class: "input") })
add.("form_with_post_no_model", v.form_with(url: "/s", class: "k") { |f| f.text_field(:q) + f.text_area(:body) })
add.("form_with_nested", v.form_with(url: "/s", scope: :account) { |f| f.fields_for(:settings) { |g| g.text_field(:a, value: "1") } })
add.("hidden_field_tag", v.hidden_field_tag("a[b]", "v<", class: "c"))
add.("hidden_field_tag_nil", v.hidden_field_tag("q", nil))
add.("button_tag", v.button_tag("x<", class: "b", data: { a: 1 }))
add.("asset_path", v.asset_path("arrow-left.svg"))

File.write(File.join(ARGV.fetch(0), "helper_golden.json"), JSON.pretty_generate({ "signed_stream_name" => signed, "cases" => cases }) + "\n")
