# C++ HTTP/1.1 Server

A non-blocking, multi-threaded HTTP/1.1 server built from scratch in modern C++ (C++20), using `epoll` for I/O multiplexing and a thread pool for request processing. Built as a systems-programming deep dive into how production HTTP servers handle concurrency, protocol framing, and abuse resistance — not a wrapper around an existing library.

No external HTTP/networking dependencies. Raw sockets, raw `epoll`, hand-written HTTP/1.1 parsing and encoding.

---

## Why this project

Most "build an HTTP server" projects stop at "it can serve a GET request over a blocking socket." This one is built around the actual hard parts of the protocol and concurrent I/O:

* **Partial reads** — a request can arrive split across an arbitrary number of `recv()` calls, and the parser must resume correctly from any split point.

* **Two body-framing strategies** — `Content-Length` and `Transfer-Encoding: chunked`, with framing handled explicitly rather than relying on the transport layer.

* **Abuse resistance** at three independent layers: malformed-input rejection, per-IP request rate limiting, and per-IP/global connection admission control.

* Correct **edge-triggered `epoll`** semantics, including a subtle admission-control recovery problem that naive implementations can miss.

---

### Request lifecycle, end to end

1. **`epoll_wait()`** blocks until a registered file descriptor becomes ready for an event such as accept, read, or write.

2. **`handle_accept()`** drains the listening socket, extracts the client IP, checks the **global connection cap** and **per-IP connection cap**, then registers the client socket with `EPOLLIN | EPOLLET | EPOLLONESHOT`.

3. **`handle_client_read()`** submits request-processing work to the **thread pool** so the epoll thread does not perform expensive parsing or handler logic.

4. A worker thread drains the socket and feeds bytes into **`HttpRequestParser`** — an incremental state machine that can pause and resume mid-request-line, mid-header, or mid-chunk across multiple `recv()` calls.

5. Once a request is successfully parsed, the **rate limiter** (token bucket, per source IP) is checked before handler logic runs. Rejected requests receive a `429` response and the connection is closed.

6. **`HttpRouter::route()`** dispatches the request to the matching handler, producing an `HttpResponse`.

7. **`HttpResponseEncoder`** serializes the response and applies the appropriate HTTP body-framing strategy. The encoded bytes are placed into the connection's write buffer. `flush()` only handles sending queued bytes and does not perform HTTP framing.

8. **`EPOLLONESHOT`** is re-armed for the connection. `EPOLLIN` is enabled for further reads, and `EPOLLOUT` is added when queued response data still needs to be sent. This prevents multiple workers from processing the same connection concurrently.

9. A periodic tick in the main loop (`check_idle_timeouts()`) removes idle connections and performs rate-limiter maintenance. No separate timer thread is required.

---

## Architecture Deep Dive: Three Things That Are Easy to Get Wrong

### 1. HTTP body framing must be unambiguous

`Content-Length` and `Transfer-Encoding: chunked` represent different body-framing mechanisms and must not be used together in an ambiguous HTTP message.

Ambiguous framing can create request-smuggling vulnerabilities.

The `HttpResponseEncoder` is responsible for determining the response framing and ensuring that the generated response does not contain conflicting framing headers.

For requests, the parser must independently validate conflicting `Content-Length` and `Transfer-Encoding` headers.

---

### 2. Chunked parsing is a state machine, not a simple loop

A chunked request body can legally arrive split at **any byte boundary**:

* in the middle of the chunk-size line
* in the middle of chunk data
* between bytes of the terminating CRLF
* in the trailer section

The `HttpRequestParser` therefore uses explicit states:

```text
ChunkSize
    |
    v
ChunkData
    |
    v
ChunkCRLF
    |
    v
ChunkSize
    |
    ...
    |
    v
Trailers
    |
    v
Complete
```

Each state consumes the required input using primitives such as `extractLine()` or `extractBytes()`.

If insufficient data is available, parsing returns `Incomplete` and resumes when the next `recv()` provides more bytes.

The implementation is tested by fragmenting chunked requests byte-by-byte across the socket to verify correct behavior under arbitrary TCP fragmentation.

---

### 3. Edge-triggered epoll + admission control has a recovery trap

With edge-triggered `epoll`, the listening socket can become readable while the connection limit is already reached.

If `handle_accept()` stops draining the listening socket at that point, the server cannot rely on another edge-triggered notification after an existing connection is released. The listening socket may remain readable without producing a new readiness edge.

This can cause the accept loop to become stuck even though connection capacity is available again.

The implementation handles this by having `remove_connection()` trigger another accept attempt after releasing a connection slot.

This gives the server an explicit recovery path when capacity becomes available again.

---

## Features

| Layer                            | Feature                                                                                                                |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| **I/O**                          | `epoll` (edge-triggered), non-blocking sockets, `EPOLLONESHOT`                                                         |
| **Concurrency**                  | Thread pool for request processing                                                                                     |
| **HTTP parsing**                 | Incremental state-machine parser, resumable across arbitrary TCP fragmentation                                         |
| **Body framing**                 | `Content-Length` and `Transfer-Encoding: chunked`                                                                      |
| **Malformed input**              | Invalid chunk sizes, integer overflow, oversized chunks, broken CRLF framing, and other malformed input return `400`   |
| **Rate limiting**                | Per-IP token bucket with steady refill rate and burst capacity                                                         |
| **Connection admission control** | Per-IP connection cap and global connection cap                                                                        |
| **Idle connection reaping**      | Periodic sweep closes connections idle past a configured timeout                                                       |
| **Graceful shutdown**            | Stops accepting new connections, drains in-flight writes, and force-closes connections after a bounded shutdown period |

---

## Testing

Every feature above was verified with targeted test scripts using raw Python sockets rather than relying only on manual `curl` testing.

* **Chunked encode/decode round-trip**, including byte-by-byte fragmented delivery to exercise partial-read code paths.

* **Malformed chunked input**: invalid hexadecimal values, empty size lines, missing CRLF, oversized chunks, integer overflow, and negative-looking sizes — confirmed to return `400`.

* **Rate limiter**: burst allowance, rejection after capacity is exhausted, and token refill/recovery after a cooldown period.

* **Connection admission control**: per-IP connection-cap enforcement and recovery after hitting the global connection cap under edge-triggered `epoll`.

---

## Project Structure

```text
include/
├── Http/
│   ├── HttpRequest.h
│   ├── HttpResponse.h
│   ├── HttpRequestParser.h
│   ├── HttpResponseEncoder.h
│   └── HttpRouter.h
│
├── protocol/
│   └── Framer.h
│
├── network/
│   └── Connection.h
│
├── middleware/
│   ├── RateLimiter.h
│   └── PerIpConnectionLimiter.h
│
├── concurrency/
│   └── ThreadPool.h
│
└── Utils/
    ├── Logger.h
    └── HttpLogGuard.h

src/
├── Http/
│   ├── HttpRequest.cpp
│   ├── HttpResponse.cpp
│   ├── HttpRequestParser.cpp
│   ├── HttpResponseEncoder.cpp
│   └── HttpRouter.cpp
│
├── protocol/
│   └── Framer.cpp
│
├── network/
│   └── Connection.cpp
│
├── middleware/
│   ├── RateLimiter.cpp
│   └── PerIpConnectionLimiter.cpp
│
├── concurrency/
│   └── ThreadPool.cpp
│
└── Utils/
    ├── Logger.cpp
    └── HttpLogGuard.cpp

main.cpp
```

---

## Build & Run

```bash
g++ -std=c++20 -Wall -Wextra -pthread -Iinclude main.cpp src/*/*.cpp -o tcp_server

./tcp_server
```

---

## Roadmap

* [ ] TLS support with OpenSSL
* [ ] Range requests / partial content (`206`) for static file serving
* [ ] HTTP/1.0 vs HTTP/1.1 keep-alive semantics audit
* [ ] Request-side `Content-Length` + `Transfer-Encoding` smuggling guard
* [ ] Benchmark writeup: requests/sec vs thread pool size
* [ ] More comprehensive HTTP compliance testing

---

## License

MIT
