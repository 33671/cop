# Async HTTP SSE Client in C

An asynchronous Server-Sent Events (SSE) client using **libmill** (coroutines) and **libcurl**.

## Architecture

```
                       Main Process (libmill)
  ┌─────────────────┐     ┌─────────────────┐
  │  SSE Coroutine  │     │  SSE Coroutine  │    ...
  │  fdwait(pipe)   │     │  fdwait(pipe)   │
  └────────┬────────┘     └────────┬────────┘
           │                       │
           └───────────────────────┘
                       │
              ┌────────┴────────┐
              │    Pipe (fd)    │
              │  [read]←[write] │
              └────────┬────────┘
                       │
  ┌────────────────────┼────────────────────┐
  │   Child Process    │   Child Process    │
  │ curl_easy_perform()│ curl_easy_perform()│
  └────────────────────┘────────────────────┘
                       │
                       ▼
                 HTTP Server
```

## Files

| File | Description |
|------|-------------|
| `sse_client_basic.c` | Basic blocking SSE client (libcurl only) |
| `sse_client_libmill.c` | Async SSE client (libmill + libcurl with pipe) |
| `test_server.py` | Python test server for SSE |
| `libmill/` | libmill async library |

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Running

### 1. Start the test server (in one terminal):
```bash
cd /home/zhang/async_c_http
python3 test_server.py 5344
```

### 2. Run the basic client:
```bash
./build/sse_client_basic
# or with custom URL:
./build/sse_client_basic http://example.com/events
```

### 3. Run the async libmill client:
```bash
./build/sse_client_libmill
# or with custom URL:
./build/sse_client_libmill http://example.com/events

# Run multiple connections:
./build/sse_client_libmill http://127.0.0.1:5344/stream 3
```

## Key Features

### Async Architecture
- **libcurl** runs in a separate thread for each connection
- **pipe** transfers data from curl thread to libmill coroutine
- **fdwait()** allows libmill to wait asynchronously on the pipe
- Multiple SSE connections can run concurrently

### SSE Parsing
- Full SSE protocol support (data, event, id fields)
- Multi-line data support
- Event dispatching with callbacks
- Proper handling of comments and empty lines

## Dependencies

- `libcurl` - HTTP client library
- `libmill` - Included in `libmill/`
- `pthread` - POSIX threads
- Python 3 with `aiohttp` (for test server)

## API Overview

```c
// Start an async SSE client
sse_client_t *sse_client_start(
    const char *url,
    void (*on_event)(const sse_event_t *, sse_client_t *),
    void *user_data
);

// Stop the client
void sse_client_stop(sse_client_t *client);

// Event handler callback
void my_handler(const sse_event_t *event, sse_client_t *client) {
    printf("Type: %s\n", event->event_type);
    printf("Data: %s\n", event->data);
    printf("ID: %s\n", event->id);
}
```

## License

MIT - See libmill/LICENSE for details
