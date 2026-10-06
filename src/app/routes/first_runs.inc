# FirstRunsController. Rails: config/routes.rb `resource :first_run` (only show and create exist in the controller).
GET    /first_run/new(.:format)  misc::action_not_found
GET    /first_run/edit(.:format)  misc::action_not_found
GET    /first_run(.:format)  first_runs::show
PATCH  /first_run(.:format)  misc::action_not_found
PUT    /first_run(.:format)  misc::action_not_found
DELETE /first_run(.:format)  misc::action_not_found
POST   /first_run(.:format)  first_runs::create
