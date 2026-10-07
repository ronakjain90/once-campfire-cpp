# Active Storage. Rails: activestorage/config/routes.rb (the order of the engine: the redirect and proxy routes first).
GET  /rails/active_storage/blobs/redirect/:signed_id/*filename                              active_storage::blobs_redirect
GET  /rails/active_storage/blobs/proxy/:signed_id/*filename                                 active_storage::blobs_proxy
GET  /rails/active_storage/blobs/:signed_id/*filename                                       active_storage::blobs_redirect
GET  /rails/active_storage/representations/redirect/:signed_blob_id/:variation_key/*filename active_storage::representations_redirect
GET  /rails/active_storage/representations/proxy/:signed_blob_id/:variation_key/*filename    active_storage::representations_proxy
GET  /rails/active_storage/representations/:signed_blob_id/:variation_key/*filename         active_storage::representations_redirect
GET  /rails/active_storage/disk/:encoded_key/*filename                                      active_storage::disk_show
PUT  /rails/active_storage/disk/:encoded_token(.:format)                                              active_storage::disk_update
POST /rails/active_storage/direct_uploads(.:format)                                                   active_storage::direct_uploads_create
