# SessionsController. Rails: config/routes.rb `resource :session` (a singular resource: the actions that the controller does not define are 404).
GET    /session/new(.:format)  sessions::new_
GET    /session/edit(.:format)  misc::action_not_found
GET    /session(.:format)  misc::action_not_found
PATCH  /session(.:format)  misc::action_not_found
PUT    /session(.:format)  misc::action_not_found
POST   /session(.:format)      sessions::create
DELETE /session(.:format)      sessions::destroy
# Sessions::TransfersController. Rails: `resource :session do scope module: "sessions" do resources :transfers, only: %i[ show update ]`.
GET    /session/transfers/:id(.:format)  session_transfers::show
PATCH  /session/transfers/:id(.:format)  session_transfers::update
PUT    /session/transfers/:id(.:format)  session_transfers::update
