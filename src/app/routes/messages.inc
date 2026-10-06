# MessagesController, the write side. Rails: config/routes.rb `resources :rooms { resources :messages }` and
# `resources :messages`. `index` is the route of the rooms area (A2). The bot routes have the default format json.
POST   /rooms/:room_id/messages(.:format)                    messages_controller::create
GET    /rooms/:room_id/messages/new(.:format)                misc::action_not_found
GET    /rooms/:room_id/messages/:id/edit(.:format)           messages_controller::edit
GET    /rooms/:room_id/messages/:id(.:format)                messages_controller::show
PATCH  /rooms/:room_id/messages/:id(.:format)                messages_controller::update
PUT    /rooms/:room_id/messages/:id(.:format)                messages_controller::update
DELETE /rooms/:room_id/messages/:id(.:format)                messages_controller::destroy
# Messages::Boosts::ByBotsController and Messages::ByBotsController. Rails: `scope path: ":bot_key", as: :bot, defaults: { format: :json }`.
POST   /rooms/:room_id/:bot_key/messages/:message_id/boosts(.:format)      messages_boosts_by_bots::create format=json
DELETE /rooms/:room_id/:bot_key/messages/:message_id/boosts/:id(.:format)  messages_boosts_by_bots::destroy format=json
GET    /rooms/:room_id/:bot_key/messages(.:format)           messages_by_bots::index format=json
POST   /rooms/:room_id/:bot_key/messages(.:format)           messages_by_bots::create format=json
PATCH  /rooms/:room_id/:bot_key/messages/:id(.:format)       messages_by_bots::update format=json
PUT    /rooms/:room_id/:bot_key/messages/:id(.:format)       messages_by_bots::update format=json
DELETE /rooms/:room_id/:bot_key/messages/:id(.:format)       messages_by_bots::destroy format=json
# Messages::BoostsController. Rails: `resources :messages { scope module: "messages" { resources :boosts } }`.
GET    /messages/:message_id/boosts(.:format)                messages_boosts::index
POST   /messages/:message_id/boosts(.:format)                messages_boosts::create
GET    /messages/:message_id/boosts/new(.:format)            messages_boosts::new_
GET    /messages/:message_id/boosts/:id/edit(.:format)       misc::action_not_found
GET    /messages/:message_id/boosts/:id(.:format)            misc::action_not_found
PATCH  /messages/:message_id/boosts/:id(.:format)            misc::action_not_found
PUT    /messages/:message_id/boosts/:id(.:format)            misc::action_not_found
DELETE /messages/:message_id/boosts/:id(.:format)            messages_boosts::destroy
# MessagesController without a room. `GET /messages` is `messages#index` of the rooms area (A2).
POST   /messages(.:format)                                   messages_controller::create
GET    /messages/new(.:format)                               misc::action_not_found
GET    /messages/:id/edit(.:format)                          messages_controller::edit
GET    /messages/:id(.:format)                               messages_controller::show
PATCH  /messages/:id(.:format)                               messages_controller::update
PUT    /messages/:id(.:format)                               messages_controller::update
DELETE /messages/:id(.:format)                               messages_controller::destroy
# Autocompletable::UsersController. Rails: `namespace :autocompletable { resources :users, only: :index }`.
GET    /autocompletable/users(.:format)                      autocompletable_users_controller::index
