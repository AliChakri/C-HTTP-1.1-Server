#pragma once

#include <fcntl.h>
#include <unistd.h>

class SocketUtils
{
public:
    // Sets a socket file descriptor to non-blocking mode
    static bool set_non_blocking(int fd);
};