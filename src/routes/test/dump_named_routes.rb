# Writes named_routes.json: each named route of config/routes.rb with its path spec, endpoint and
# defaults, as the reference app reports them. The routes test compares src/routes with it.
# Run in the campfire-reference image (see the header of routes_test.cpp).
require "json"

routes = Rails.application.routes.routes.select { |r| r.name && r.defaults[:controller] }.map do |r|
  defaults = r.defaults.reject { |k, _| %i[controller action].include?(k) }
  {
    name: r.name,
    path: r.path.spec.to_s.sub("(.:format)", ""),
    endpoint: "#{r.defaults[:controller]}##{r.defaults[:action]}",
    defaults: defaults.transform_keys(&:to_s).transform_values(&:to_s),
  }
end
app = routes.reject do |r|
  r[:endpoint].start_with?("action_mailbox/", "active_storage/", "turbo/") ||
    (r[:endpoint].start_with?("rails/") && r[:name] != "rails_health_check")
end
File.write(ARGV.fetch(0), JSON.pretty_generate(app) + "\n")
