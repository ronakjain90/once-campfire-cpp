# Routes for the router tests.
GET    /                                   test::root
GET    /rooms                              test::rooms_index
POST   /rooms                              test::rooms_create
GET    /rooms/opens                        test::rooms_opens
GET    /rooms/:id(.:format)                test::rooms_show
GET    /rooms/:room_id/messages(.:format)  test::messages_index
GET    /rooms/:room_id/@:message_id        test::at_message
GET    /rooms/:room_id/:bot_key/messages(.:format)  test::bot_messages  format=json
GET    /webmanifest(.:format)              test::manifest
GET    /assets/*path                       test::asset
GET    /users/:user_id/profile             test::profile  user_id=me
