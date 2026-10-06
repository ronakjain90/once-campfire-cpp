# Rooms. Rails: config/routes.rb `resources :rooms`, `namespace :rooms`. The `show` of a room needs messages (A3).
GET /rooms                rooms::index
GET /rooms/opens/:id      rooms_opens::show
GET /rooms/closeds/:id    rooms_closeds::show
