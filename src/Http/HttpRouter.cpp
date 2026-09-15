#include "Http/HttpRouter.h"

std::string HttpRouter::make_route_key(HttpMethod method, const std::string& path)
{
    std::string key;
    switch (method)
    {
        case HttpMethod::GET:       key = "GET:"; break;
        case HttpMethod::POST:      key = "POST:"; break;
        case HttpMethod::PUT:       key = "PUT:"; break;
        case HttpMethod::DELETE:    key = "DELETE:"; break;
        case HttpMethod::HEAD:      key = "HEAD:"; break;
        case HttpMethod::OPTIONS:   key = "OPTIONS:"; break;
        default:                    key = "UNKNOWN:"; break;
    }
    key += path;
    return key;
}

void HttpRouter::add_route(HttpMethod method, const std::string& path, HttpHandler handler)
{
    std::string key = make_route_key(method, path);
    if (m_routes.find(key) != m_routes.end())
    {
        LOG_WARN(-1, "Duplicate Route in: " + std::string(key));
    }
    m_routes[key] = std::move(handler);
}

void HttpRouter::get(const std::string& path, HttpHandler handler)
{
    add_route(HttpMethod::GET, path, handler);
}

void HttpRouter::post(const std::string& path, HttpHandler handler) 
{
    add_route(HttpMethod::POST, path, std::move(handler));
}

void HttpRouter::put(const std::string& path, HttpHandler handler) 
{
    add_route(HttpMethod::PUT, path, std::move(handler));
}

void HttpRouter::del(const std::string& path, HttpHandler handler) 
{
    add_route(HttpMethod::DELETE, path, std::move(handler));
}

HttpResponse HttpRouter::route(const HttpRequest& request) const
{
    std::string key = make_route_key(request.method, request.path);

    auto it = m_routes.find(key);
    if (it != m_routes.end())
    {
        // Execute the matched route handler function
        return it->second(request);
    }

    // Default 404 Not Found fallback if no route matched
    HttpResponse not_found_res;
    not_found_res.set_status(404, "Not Found")
                            .html("<h1>404 Not Found</h1><p>The requested URL was not found on this server.</p>");
    
    return not_found_res;
}