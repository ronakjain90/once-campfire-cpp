# Users: show, avatar, ban, profile, push subscriptions. Rails: config/routes.rb `resources :users, only: :show`. `user_id=me` is `scope defaults: { user_id: "me" }`.
GET    /users/:user_id/avatar(.:format)  users_avatars::show
DELETE /users/:user_id/avatar(.:format)  users_avatars::destroy
