# PwaController. Rails: config/routes.rb `get "webmanifest"` and `get "service-worker"`.
GET /webmanifest(.:format)    pwa::manifest
GET /service-worker(.:format) pwa::service_worker
