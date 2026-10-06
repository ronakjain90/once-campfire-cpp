# Users::SidebarsController. Rails: config/routes.rb `resources :users { resource :sidebar }` (user_id is "me").
GET /users/:user_id/sidebar users_sidebars::show
