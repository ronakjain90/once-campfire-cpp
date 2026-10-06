# UsersController#new and #create. Rails: config/routes.rb `get "join/:join_code"`, `post "join/:join_code"`.
GET    /join/:join_code(.:format)  join::new_
POST   /join/:join_code(.:format)  join::create
