# Action Cable. Rails: ActionCable::Server::Base mounted at `/cable` (config/routes.rb). Every method reaches it; anything
# that is not a WebSocket upgrade from an allowed origin gets "Page not found".
GET     /cable  cable_endpoint::show
HEAD    /cable  cable_endpoint::show
POST    /cable  cable_endpoint::show
PUT     /cable  cable_endpoint::show
PATCH   /cable  cable_endpoint::show
DELETE  /cable  cable_endpoint::show
OPTIONS /cable  cable_endpoint::show
