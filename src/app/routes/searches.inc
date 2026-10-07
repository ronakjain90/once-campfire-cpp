# SearchesController. Rails: config/routes.rb `resources :searches, only: %i[ index create ] { delete :clear, on: :collection }`.
DELETE /searches/clear(.:format)  searches::clear
GET    /searches(.:format)        searches::index
POST   /searches(.:format)        searches::create
