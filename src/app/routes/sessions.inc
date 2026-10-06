# SessionsController. Rails: config/routes.rb `resource :session`.
GET    /session/new  sessions::new_
POST   /session      sessions::create
DELETE /session      sessions::destroy
