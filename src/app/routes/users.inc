# Users: show, avatar, ban, profile, push subscriptions. Rails: config/routes.rb `resources :users, only: :show`. `user_id=me` is `scope defaults: { user_id: "me" }`.
GET    /users/:user_id/avatar(.:format)  users_avatars::show
DELETE /users/:user_id/avatar(.:format)  users_avatars::destroy
DELETE /users/:user_id/ban(.:format)  users_bans::destroy
POST   /users/:user_id/ban(.:format)  users_bans::create
GET    /users/:user_id/profile/new(.:format)  misc::action_not_found  user_id=me
GET    /users/:user_id/profile/edit(.:format)  misc::action_not_found  user_id=me
GET    /users/:user_id/profile(.:format)  users_profiles::show  user_id=me
PATCH  /users/:user_id/profile(.:format)  users_profiles::update  user_id=me
PUT    /users/:user_id/profile(.:format)  users_profiles::update  user_id=me
DELETE /users/:user_id/profile(.:format)  misc::action_not_found  user_id=me
POST   /users/:user_id/profile(.:format)  misc::action_not_found  user_id=me
POST   /users/:user_id/push_subscriptions/:push_subscription_id/test_notifications(.:format)  users_test_notifications::create  user_id=me
GET    /users/:user_id/push_subscriptions(.:format)  users_push_subscriptions::index  user_id=me
POST   /users/:user_id/push_subscriptions(.:format)  users_push_subscriptions::create  user_id=me
GET    /users/:user_id/push_subscriptions/new(.:format)  misc::action_not_found  user_id=me
GET    /users/:user_id/push_subscriptions/:id/edit(.:format)  misc::action_not_found  user_id=me
GET    /users/:user_id/push_subscriptions/:id(.:format)  misc::action_not_found  user_id=me
PATCH  /users/:user_id/push_subscriptions/:id(.:format)  misc::action_not_found  user_id=me
PUT    /users/:user_id/push_subscriptions/:id(.:format)  misc::action_not_found  user_id=me
DELETE /users/:user_id/push_subscriptions/:id(.:format)  users_push_subscriptions::destroy  user_id=me
GET    /users/:id(.:format)  users::show
