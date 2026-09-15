
#include "network/TcpServer.h"

TcpServer::TcpServer(uint16_t port_num) :   port(port_num),
                                            router(),
                                            task_queue(), 
                                            thread_pool(&task_queue)
{
}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    // create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR(server_fd, "Failed to Create Socket.");
        return false;
    }

    // set Socket options
    int opt = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN(server_fd, "setsockopt SO_REUSEADDR failed.");
    }

    // Set server listening socket to non-blocking mode
    SocketUtils::set_non_blocking(server_fd);

    // Configure Ip Header
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // we type cast our ip header and bind it
    auto generic_addr_ptr = reinterpret_cast<struct sockaddr*> (&server_addr);
    if (bind(server_fd, generic_addr_ptr, sizeof(server_addr)) < 0) {
        LOG_ERROR(server_fd, "Failed to Bind Socket.");
        return false;
    }

    // listen For Reqs
    if (listen(server_fd, 256) < 0) {
        LOG_ERROR(server_fd, "Listen Failed");
        return false;
    }

    // Initialize EventLoop with non-blocking server socket and ThreadPool
    event_loop = std::make_unique<EventLoop>(server_fd, thread_pool, router);
    if (!event_loop->start())
    {
        return false;
    }

    is_running = true;
    
    return true;
}

void TcpServer::run() {
    if (is_running && event_loop) {
        // Event loop handles socket readiness; thread pool handles execution tasks
        event_loop->run();
    }
}

void TcpServer::stop() {
    if (is_running)
    {
        is_running = false;

        //  Stop event loop (drains write buffers on active client connections)
        if (event_loop) {
            event_loop->stop();
        }
    }
}
