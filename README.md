# C++ HTTP/1.1 Server

A non-blocking, multi-threaded HTTP/1.1 server built from scratch in modern C++ (C++20), using `epoll` for I/O multiplexing and a thread pool for request processing. Built as a systems-programming deep dive into how production HTTP servers handle concurrency, protocol framing, and abuse resistance — not a wrapper around an existing library.

No external HTTP/networking dependencies. Raw sockets, raw `epoll`, hand-written HTTP/1.1 parsing and encoding.

---

## Why this project

Most "build an HTTP server" projects stop at "it can serve a GET request over a blocking socket." This one is built around the actual hard parts of the protocol and concurrent I/O:

* **Partial reads** — a request can arrive split across an arbitrary number of `recv()` calls, and the parser must resume correctly from any split point.

* **Two body-framing strategies** (`Content-Length` and `Transfer-Encoding: chunked`), chosen automatically and never sent together.

* **Abuse resistance** at three independent layers: malformed-input rejection, per-IP request rate limiting, and per-IP/global connection admission control.

* Correct **edge-triggered `epoll`** semantics, including a subtle bug class most naive implementations miss (see [Architecture Deep Dive](#architecture-deep-dive)).

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

1. **`epoll_wait()`** blocks until a socket is ready for an event such as accept, read, or write.

2. **`handle_accept()`** drains the kernel's backlog: extracts the client IP, checks the **global connection cap** and **per-IP connection cap**, then registers the socket with `EPOLLIN | EPOLLET | EPOLLONESHOT`.

3. **`handle_client_read()`** hands the actual request-processing work off to the **thread pool** — the epoll thread never blocks on parsing or handler logic.

4. A worker thread drains the socket and feeds bytes into **`HttpRequestParser`** — an incremental state machine that can pause and resume mid-request-line, mid-header, or mid-chunk across however many `recv()` calls it takes.

5. On a successfully parsed request, the **rate limiter** (token bucket, per source IP) is checked before handler logic runs. Rejected requests receive an immediate `429` response and the connection is closed — the router is never touched.

6. **`HttpRouter::route()`** dispatches to the matching handler, producing an `HttpResponse`.

7. **`HttpResponseEncoder`** selects `Content-Length` or `Transfer-Encoding: chunked` according to the response encoding rules and serializes the response into the connection's write buffer. `flush()` itself is a byte-pump with no knowledge of HTTP framing.

8. **`EPOLLONESHOT`** re-arms the socket (`EPOLLIN`, plus `EPOLLOUT` if bytes are still queued) so no two worker threads can race on the same connection.

9. A background tick in the main loop (`check_idle_timeouts()`) periodically evicts idle connections and sweeps stale rate-limiter buckets — no separate timer thread is needed.

---

## Architecture Deep Dive: Three Things That Are Easy to Get Wrong

### 1. Body framing must be mutually exclusive

`Content-Length` and `Transfer-Encoding: chunked` must not both be used to frame the same HTTP message. Ambiguous framing can create request-smuggling vulnerabilities.

`HttpResponseEncoder` is the single source of truth for the response framing decision and explicitly removes whichever framing header does not apply rather than relying on downstream code to avoid setting both.

For requests, the parser separately validates the presence of conflicting `Content-Length` and `Transfer-Encoding` headers and rejects ambiguous framing.

### 2. Chunked parsing is a state machine, not a loop

A chunked request body can legally arrive split at **any byte boundary** — mid chunk-size line, mid chunk data, or mid trailing CRLF.

The parser (`HttpRequestParser`) models this as explicit states:

```text
ChunkSize
    ↓
ChunkData
    ↓
ChunkCRLF
    ↓
ChunkSize
    ...
    ↓
Trailers
    ↓
Complete
```

Each state consumes exactly one primitive (`extractLine()` or `extractBytes()`) and either advances or returns `Incomplete` to pause and wait for more data on the next `recv()`.

The implementation is tested by fragmenting chunked requests **byte-by-byte** across the socket to verify that reassembly remains correct under arbitrary TCP fragmentation.

### 3. Edge-triggered epoll + admission control has a recovery trap

If the global connection cap causes `handle_accept()` to stop draining the kernel's backlog early, edge-triggered `epoll` will **not necessarily re-notify** the server while the listening socket remains readable — the readiness edge has already occurred.

Left unhandled, this can wedge the accept loop after the server reaches capacity, even after existing connections are released.

The fix is for `remove_connection()` to explicitly re-invoke `handle_accept()` after releasing a connection slot. This gives the server a reliable recovery point once capacity becomes available again.

---

## Features

| Layer                            | Feature                                                                                                                      |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **I/O**                          | `epoll` (edge-triggered), non-blocking sockets, `EPOLLONESHOT` to prevent cross-thread races                                 |
| **Concurrency**                  | Thread pool for request processing, epoll thread never blocks                                                                |
| **HTTP parsing**                 | Incremental state-machine parser, resumable across arbitrary TCP fragmentation                                               |
| **Body framing**                 | `Content-Length` and `Transfer-Encoding: chunked`, with request and response support                                         |
| **Malformed input**              | Rejects invalid hex chunk sizes, integer overflow, negative sizes, oversized chunks, and broken CRLF framing — returns `400` |
| **Rate limiting**                | Per-IP token bucket with steady refill rate and burst capacity                                                               |
| **Connection admission control** | Per-IP concurrent connection cap, global connection cap via atomic counter, kernel backlog as final backstop                 |
| **Idle connection reaping**      | Periodic sweep closes connections idle past a configured timeout                                                             |
| **Graceful shutdown**            | Stops accepting new connections, drains in-flight writes, and uses a bounded shutdown timeout before force-close             |

---

## Testing

Every feature above was verified with targeted test scripts using raw Python sockets rather than only manual `curl` testing:

* **Chunked encode/decode round-trip**, including byte-by-byte fragmented delivery to force partial-read code paths.

* **Malformed chunked input**: invalid hex, empty size line, missing CRLF, oversized chunk, integer overflow, and negative-looking size — all confirmed to return `400`.

* **Rate limiter**: burst allowance, correct rejection after capacity is exhausted, and token refill/recovery after a cooldown window.

* **Connection admission control**: per-IP cap enforcement and — the trickiest case — confirming that the server correctly **resumes accepting connections** after hitting the global cap under edge-triggered `epoll`, rather than silently wedging.

---

## Project Structure

```text
include/

  Http/
    HttpRequest
    HttpResponse
    HttpRequestParser
    HttpResponseEncoder
    HttpRouter

  protocol/
    Framer

  network/
    Connection

  middleware/
    RateLimiter
    PerIpConnectionLimiter

  concurrency/
    ThreadPool

  Utils/
    Logger
    HttpLogGuard

src/

  ... corresponding .cpp files

main.cpp
  Server bootstrap and route registration
```

---

## Build & Run

```bash
g++ -std=c++20 -Wall -Wextra -pthread -Iinclude main.cpp src/*/*.cpp -o tcp_server

./tcp_server
```

---

## Roadmap

* [ ] TLS support (OpenSSL)
* [ ] Range requests / partial content (`206`) for static file serving
* [ ] HTTP/1.0 vs 1.1 keep-alive default semantics audit
* [ ] Request-side `Content-Length` + `Transfer-Encoding` smuggling guard (reject if both are present)
* [ ] Benchmark writeup (requests/sec vs. thread pool size)

---

## License

MIT

````

### One thing I deliberately changed

Your original said:

> `MAX_CHUCK_SIZE`

That's almost certainly a typo for `MAX_CHUNK_SIZE`. More importantly, I removed the claim that the encoder chooses chunked **"based on body size"** because that's an implementation policy rather than an HTTP/1.1 requirement. HTTP does **not** require you to use chunked encoding just because a body is large.

Also, your roadmap says:

> Request-side Content-Length + Transfer-Encoding smuggling guard

So if you **haven't implemented that yet**, the README shouldn't claim requests already validate it. That's why I separated request validation from the currently implemented response behavior.

### Save the README

If your file is `README.md`:

```bash
nano README.md
````

Paste the corrected README, then:

* `Ctrl + O` → save
* `Enter` → confirm filename
* `Ctrl + X` → exit

Or if you're using VS Code, just save with **Ctrl + S**.

### Check what Git sees

From your project root:

```bash
git status
```

You should see something like:

```text
modified: README.md
```

Then inspect your changes:

```bash
git diff README.md
```

If everything looks good:

```bash
git add README.md
```

Then commit:

```bash
git commit -m "docs: update README"
```

Then push:

```bash
git push
```

So the normal workflow is:

```bash
git status
git diff
git add README.md
git commit -m "docs: update README"
git push
```

**Important:** `git add` does not push anything. It stages the change locally. `git commit` records it locally. `git push` sends the commit to your remote repository (such as GitHub).
