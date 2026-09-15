#pragma once

#include "Http/HttpRequest.h"
#include "Http/HttpResponse.h"
#include "Utils/Logger.h"

#include <functional>
#include <unordered_map>
#include <string>

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpRouter
{
private:
    // Internal lookup table mapping "METHOD:PATH" -> Handler function
    std::unordered_map<std::string, HttpHandler> m_routes;

    // Helper to generate unique key for internal map
    static std::string make_route_key(HttpMethod method, const std::string& path);
public:
    HttpRouter() = default;

    // Route Registration Helpers
    void add_route(HttpMethod method, const std::string& path, HttpHandler handler);
    void get(const std::string& path,HttpHandler handler);
    void post(const std::string& path,HttpHandler handler);
    void put(const std::string& path,HttpHandler handler);
    void del(const std::string& path,HttpHandler handler);

    // Matches incoming request to a registered handler and executes it
    HttpResponse route(const HttpRequest& request) const;
};
