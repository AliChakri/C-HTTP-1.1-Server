# C++ HTTP/1.1 Server

A non-blocking, multi-threaded HTTP/1.1 server built from scratch in modern C++ (C++20), using `epoll` for I/O multiplexing and a thread pool for request processing. Built as a systems-programming deep dive into how production HTTP servers actually handle concurrency, protocol framing, and abuse — not a wrapper around an existing library.

No external HTTP/networking dependencies. Raw sockets, raw `epoll`, hand-written HTTP/1.1 parsing and encoding.

---

## Why this project

Most "build an HTTP server" projects stop at "it can serve a GET request over a blocking socket." This one is built around the actual hard parts of the protocol and of concurrent I/O:

- Correct handling of **partial reads** — a request can arrive split across an arbitrary number of `recv()` calls, and the parser must resume correctly from any split point.
- **Two body-framing strategies** (`Content-Length` and `Transfer-Encoding: chunked`), chosen automatically and never sent together (a request-smuggling class bug if done wrong).
- **Abuse resistance** at three independent layers: malformed-input rejection, per-IP request rate limiting, and per-IP/global connection admission control.
- Correct **edge-triggered epoll** semantics, including a subtle bug class most naive implementations miss (see [Architecture Deep Dive](#architecture-deep-dive)).

---

## Architecture Overview

```mermaid
flowchart TB
    subgraph OS["Kernel"]
        BL["TCP Backlog\n(listen() queue)"]
    end

    subgraph EL["EventLoop (single thread)"]
        EP["epoll_wait()"]
        HA["handle_accept()"]
        HR["handle_client_read()"]
        HW["handle_client_write()"]
        IT["check_idle_timeouts()\n+ rate limiter sweep"]
    end

    subgraph TP["ThreadPool (worker threads)"]
        W1["Worker: parse + route + encode"]
    end

    subgraph GUARDS["Admission Control"]
        GC["Global connection cap\n(atomic counter)"]
        IC["Per-IP connection cap"]
        RL["Per-IP rate limiter\n(token bucket)"]
    end

    Client["Client"] -->|TCP handshake| BL
    BL --> EP
    EP -->|server_fd readable| HA
    HA -->|check| GC
    HA -->|check| IC
    HA -->|accept + register EPOLLONESHOT| EL

    EP -->|client_fd readable| HR
    HR -->|submit job| TP
    W1 -->|parse HTTP request| PARSE["HttpRequestParser\n(state machine)"]
    PARSE -->|Success| RL
    RL -->|allowed| ROUTE["HttpRouter::route()"]
    RL -->|denied| R429["429 response"]
    ROUTE --> ENCODE["HttpResponseEncoder::encode()"]
    ENCODE -->|Content-Length or chunked| WBUF["Connection write buffer"]
    WBUF --> EP
    EP -->|client_fd writable| HW
    HW -->|flush()| Client

    IT -.->|periodic tick| RL
    IT -.->|periodic tick| EL
```

### Request lifecycle, end to end

1. **`epoll_wait()`** blocks until a socket is ready (accept, read, or write).
2. **`handle_accept()`** drains the kernel's backlog: extracts the client IP, checks the **global connection cap** (atomic counter) and **per-IP connection cap**, then registers the socket with `EPOLLIN | EPOLLET | EPOLLONESHOT`.
3. **`handle_client_read()`** hands the actual work off to the **thread pool** — the epoll thread never blocks on parsing or handler logic.
4. A worker thread drains the socket, feeds bytes into **`HttpRequestParser`** — an incremental state machine that can pause and resume mid-request-line, mid-header, or mid-chunk across however many `recv()` calls it takes.
5. On a successfully parsed request, the **rate limiter** (token bucket, per source IP) is checked before any handler logic runs. Rejected requests get an immediate `429` and the connection is closed — the router is never touched.
6. **`HttpRouter::route()`** dispatches to the matching handler, producing an `HttpResponse`.
7. **`HttpResponseEncoder`** decides `Content-Length` vs `Transfer-Encoding: chunked` based on body size, and serializes the full response into the connection's write buffer — `flush()` itself is a dumb byte-pump with no knowledge of HTTP framing.
8. **`EPOLLONESHOT`** re-arms the socket (`EPOLLIN`, plus `EPOLLOUT` if bytes are still queued) so no two worker threads can ever race on the same connection.
9. A background tick in the main loop (`check_idle_timeouts()`) periodically evicts idle connections and sweeps stale rate-limiter buckets — no separate timer thread needed.

---

## Architecture Deep Dive: three things that are easy to get wrong

### 1. Body framing must be mutually exclusive

`Content-Length` and `Transfer-Encoding: chunked` must never both appear on the same response — sending both is a known HTTP request/response-smuggling vector. `HttpResponseEncoder::set_response_header()` is the single source of truth for this decision (based on body size vs. `MAX_CHUCK_SIZE`), and explicitly strips whichever header doesn't apply rather than relying on downstream code to "just not set the other one."

### 2. Chunked parsing is a state machine, not a loop

A chunked request body can legally arrive split at *any* byte boundary — mid chunk-size line, mid chunk data, mid trailing CRLF. The parser (`HttpRequestParser`) models this as explicit states (`ChunkSize → ChunkData → ChunkCRLF → ChunkSize → ... → Trailers → Complete`), each of which reads exactly one primitive (`extractLine` or `extractBytes`) and either advances or returns `Incomplete` to pause and wait for more bytes on the next `recv()`. Verified by fragmenting a chunked request **byte-by-byte** across the wire in tests and confirming reassembly is still correct.

### 3. Edge-triggered epoll + admission control has a recovery trap

If the global connection cap causes `handle_accept()` to stop draining the kernel's backlog early, `epoll`'s edge-triggered mode will **not** re-notify the server when the listening socket is still readable — it already fired that event once. Left unhandled, this permanently wedges the accept loop the first time the server hits capacity, even after connections free up. The fix: `remove_connection()` explicitly re-invokes `handle_accept()` after releasing a connection slot, since that's the only reliable trigger point once epoll's own notification has been "used up."

---

## Features

| Layer | Feature |
|---|---|
| **I/O** | `epoll` (edge-triggered), non-blocking sockets, `EPOLLONESHOT` to prevent cross-thread races |
| **Concurrency** | Thread pool for request processing, epoll thread never blocks |
| **HTTP parsing** | Incremental state-machine parser, resumable across arbitrary TCP fragmentation |
| **Body framing** | `Content-Length` and `Transfer-Encoding: chunked`, auto-selected by body size, request + response |
| **Malformed input** | Rejects invalid hex chunk sizes, integer overflow, negative sizes, oversized chunks, broken CRLF framing — all return `400` |
| **Rate limiting** | Per-IP token bucket (steady rate + burst capacity), sharded for low lock contention |
| **Connection admission control** | Per-IP concurrent connection cap, global connection cap via atomic counter, kernel backlog as final backstop |
| **Idle connection reaping** | Periodic sweep closes connections idle past a timeout |
| **Graceful shutdown** | Stops accepting new connections, drains in-flight writes, bounded shutdown timeout before force-close |

---

## Testing

Every feature above was verified with targeted test scripts (raw-socket Python, not just `curl`) rather than manual spot-checks:

- **Chunked encode/decode round-trip**, including byte-by-byte fragmented delivery to force partial-read code paths.
- **Malformed chunked input**: invalid hex, empty size line, missing CRLF, oversized chunk, integer overflow, negative-looking size — all confirmed to return `400`.
- **Rate limiter**: burst allowance, correct rejection past capacity, and token refill/recovery after a cooldown window.
- **Connection admission control**: per-IP cap enforcement, and — the trickiest one — confirming the server correctly *resumes* accepting connections after hitting the global cap under edge-triggered epoll, rather than silently wedging.

---

## Project Structure

```
include/
  Http/            HttpRequest, HttpResponse, HttpRequestParser, HttpResponseEncoder, HttpRouter
  protocol/         Framer (line/byte/chunk extraction primitives)
  network/          Connection (per-socket read/write buffers, state)
  middleware/        RateLimiter, PerIpConnectionLimiter
  concurrency/       ThreadPool
  Utils/            Logger, HttpLogGuard
src/
  ... corresponding .cpp files
main.cpp             Server bootstrap, route registration
```

## Build & Run

```bash
g++ -std=c++20 -Wall -Wextra -pthread -Iinclude main.cpp src/*/*.cpp -o tcp_server
./tcp_server
```

## Roadmap

- [ ] TLS support (OpenSSL)
- [ ] Range requests / partial content (206) for static file serving
- [ ] HTTP/1.0 vs 1.1 keep-alive default semantics audit
- [ ] Request-side Content-Length + Transfer-Encoding smuggling guard (reject if both present)
- [ ] Benchmark writeup (req/sec vs. thread pool size)

---

## License

MIT
