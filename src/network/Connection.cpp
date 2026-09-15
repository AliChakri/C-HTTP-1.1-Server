#include "network/Connection.h"

#include <iostream>

Connection::~Connection()
{
    if (fd >= 0) close(fd);
}

int Connection::get_fd() const {
    return fd;
}

std::vector<uint8_t>& Connection::get_read_buffer() {
    return read_buffer;
}

std::mutex& Connection::get_mutex() {
    return conn_mutex;
}

void Connection::update_activity()
{
    last_activity = std::chrono::steady_clock::now();
}

double Connection::seconds_since_last_activity()
{
    std::lock_guard<std::mutex> lock(conn_mutex);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double> (now - last_activity).count();
}

void Connection::append_unsent_data(const uint8_t* data, size_t length)
{
    write_buffer.insert(write_buffer.end(), data, data + length);
    last_activity = std::chrono::steady_clock::now();
}

void Connection::append_read_data(const uint8_t* data, size_t length)
{
    read_buffer.insert(read_buffer.end(), data, data + length);
    last_activity = std::chrono::steady_clock::now();
}

bool Connection::flush(bool& has_error)
{
    while (!write_buffer.empty())
    {
        ssize_t sent = send(fd, write_buffer.data(), write_buffer.size(), MSG_DONTWAIT | MSG_NOSIGNAL);

        if (sent > 0)
        {
            write_buffer.erase(write_buffer.begin(), write_buffer.begin() + sent);
        }
        else if (sent == 0)
        {
            return false;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return false; 
            }
            
            has_error = true;
            return false;
        }
    }

    return true; 
}

bool Connection::has_pending_writes()
{
    return !write_buffer.empty();
}