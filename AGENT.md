# AGENT.md - Project Progress Summary

## Project: async_c_http

An **async HTTP SSE (Server-Sent Events) client in C** for streaming OpenAI-compatible API responses, using **libmill** coroutines and **libcurl** threads.

---

## Architecture

```
                        Main Thread (libmill)
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
  │    Curl Thread     │    Curl Thread     │
  │ curl_easy_perform()│ curl_easy_perform()│
  └────────────────────┘────────────────────┘
                       │
                       ▼
                 HTTP Server
```

- **libcurl** runs in dedicated thread(s) for blocking HTTP I/O
- **pipe** transfers data from curl thread to libmill coroutine
- **fdwait()** enables async waiting on pipe in libmill's event loop
- Multiple concurrent SSE connections supported

---

## Component Status

| Component | Files | Status | Description |
|---|---|---|---|
| **SSE Parser** | `openai_sse_parser.h/.c` | ✅ Stable | SSE event extraction, JSON validation, chunk parsing (content, reasoning_content, tool_calls, usage) |
| **Tool Call Parser** | `tool_call_parser.h/.c` | ✅ Stable | Accumulates streaming tool_call delta fragments into complete JSON arrays |
| **Stream Client** | `openai_stream_client.h/.c` | ⚠️ Mostly stable | Async streaming client using libmill + libcurl + pipe. Lifecycle, piped data transfer, coroutine-based chunk iteration |
| **LLM Parser** | `llm_parser.h/.c` | ✅ Stable | State machine managing multi-turn conversation history (messages array), accumulates assistant responses |
| **LLM Runtime** | `llm_runtime.h/.c` | ❌ Empty (0 bytes) | **Next major component** - should tie together parser, client, and tool execution |
| **Test Program** | `openai_stream_client_async_test.c` | ⚠️ Working | Interactive REPL, sends prompts, displays streaming responses via libmill coroutines |
| **Utils** | `utils.c/.h` | ✅ Done | Simple .env file loader |
| **Build System** | `CMakeLists.txt` | ✅ Working | Builds static libs + executables |
| **Test Server** | `test_server.py` | ✅ Done | Python aiohttp SSE test server |

---

## Detailed Breakdown

### 1. SSE Parser (`openai_sse_parser.h/.c`)
- `stream_buffer_t` - Ring-buffer for accumulating raw SSE data
- `StreamChunk` - Parsed struct with: `content`, `reasoning_content`, `tool_calls`, `role`, `finish_reason`, `usage` data
- `extract_next_chunk()` - Main parsing entry point, extracts next complete chunk from buffer
- Handles `data: [DONE]` marker, multi-line JSON, SSE event boundaries

### 2. Tool Call Parser (`tool_call_parser.h/.c`)
- `ToolCallDeltaParser` - Manages up to `MAX_TOOL_CALLS` (256) parallel tool call slots
- `feed_toolcall_delta()` - Feed chunks with incremental `tool_calls` array, accumulates fragments by `index`
- Produces complete `cJSON` array of tool calls on stream end
- Handles interleaved id/name/arguments across chunks

### 3. Stream Client (`openai_stream_client.h/.c`)
- `stream_client_new/free()` - Lifecycle management
- `stream_client_start_chat()` - Launches curl thread with pipe setup.
  Accepts `cJSON *messages` (array of message objects). Builds full request
  body internally via `build_request_body()` helper using client config
  (model, temperature, system_message).
- `build_request_body()` — Static helper that constructs the complete JSON
  request body using cJSON functions (safe escaping, no raw string formatting).
  Automatically prepends system message if configured and not already present,
  and includes tool schemas (`tools` + `tool_choice: "auto"`) if set.
- `stream_client_set_tool_schemas()` — Set tool definition JSON array on the
  client. The schemas are deep-copied and included in every subsequent request
  body.
- `next_chunk()` / `next_chunk_nowait()` - Coroutine-based async chunk iteration
- `stream_client_cancel()` - Sets `running=0`, curl checks via xfer callback
- `stream_client_chat_blocking()` — Builds cJSON messages array internally
  from the prompt string, then delegates to `start_chat()`.

### 4. LLM Parser (`llm_parser.h/.c`)
- State machine: `IDLE → IN_ASSISTANT → FINISHED`
- `llm_parser_feed_chunk()` - Feeds StreamChunks, accumulates content/reasoning/tool_calls
- `llm_parser_add_message()` - Adds user/system/tool messages to history
- `llm_parser_get_history()` - Returns full `{"messages": [...]}` JSON
- Handles finish signals and usage extraction
- Status reporting: `REASONING`, `RESPONDING`, `WRITING_TOOL_CALL`, `FINISHED`

### 5. LLM Runtime (`llm_runtime.h/.c`)
- ✅ **Implemented and builds cleanly**
- High-level orchestration layer wrapping stream_client + llm_parser
- **Key API**:
  - `llm_runtime_new()` / `llm_runtime_free()` — lifecycle
  - `llm_runtime_send()` — **coroutine** that sends user message, streams response via callback, and automatically handles the tool execution loop
  - `llm_runtime_register_tool()` — register tool handlers by name
  - `llm_runtime_set_tool_schema()` — set tool definition JSON array (the
    `tools` field in the API request body). Separate from register_tool:
    set_tool_schema tells the LLM what tools exist and their signatures;
    register_tool tells the runtime which C function handles each tool.
  - `llm_runtime_add_user_message()` — add to history without sending
  - `llm_runtime_get_history()` — readonly access to full `{"messages":[...]}`
  - `llm_runtime_reset()` / `llm_runtime_cancel()` — control
- **Tool loop**: after streaming finishes, checks the last assistant message for `tool_calls`. If found, executes registered tool functions, adds `{role:"tool"}` results to parser history, and automatically sends another streaming request with the updated history. Loops up to `LLM_RUNTIME_MAX_TOOL_LOOPS` (16) times.
- **Callback events**: `REASONING`, `CONTENT`, `TOOL_CALLS`, `TOOL_RESULT`, `USAGE`, `DONE`, `ERROR`, `STATUS_CHANGE`
- **Thread safety**: cancellation via `volatile int running` flag; curl thread aborts via xfer callback

### 6. Runtime Test Program (`llm_runtime_test.c`)
- ✅ **New** interactive REPL using the high-level runtime
- Registers a `sleep` tool as example
- Colored terminal output (blue for content, dim for reasoning, green for tool results)
- Supports commands: `quit`, `reset`
- Much simpler than the old test — most complexity is inside the runtime now

### 6. Test Program (`openai_stream_client_async_test.c`)
- Interactive REPL with colored prompts
- Uses libmill coroutines: `chunk_processor` + `cancellation_monitor`
- Supports `Ctrl+C` cancellation during streaming
- Reads config from `.env` or environment variables
- **Does not yet use the LLM Parser's history for multi-turn** — each turn starts fresh

### 7. External Dependencies (included but not integrated)
| Library | Path | Purpose | Integration Status |
|---|---|---|---|
| **libmill** | `libmill/` | Coroutines / async I/O | ✅ Fully integrated |
| **cjson** | `cjson/` | JSON parsing | ✅ Fully integrated |
| **linenoise** | `linenoise/` | Line editing (history, completion) | ❌ Not yet used in any source |
| **hl-vt100** | `hl-vt100/` | VT100 terminal rendering | ❌ Not yet used in any source |

---

## What's Working

- [x] SSE stream parsing (OpenAI format with reasoning_content, tool_calls, usage)
- [x] Streaming tool call delta accumulation
- [x] Async chunk iteration via libmill coroutines + pipe + fdwait
- [x] Multi-threaded curl with backpressure (pipe write blocks)
- [x] Cancellation via xfer callback
- [x] Conversation message history management (LLM Parser)
- [x] Interactive REPL test program
- [x] Build system (CMake) for all components
- [x] .env file loading for API keys

---

## What's WIP / TODO

### Immediate (Phase 1) — ✅ Done

- [x] **Build `llm_runtime.h/.c`** — High-level runtime wrapping stream_client + llm_parser + tool_call_parser. Supports multi-turn conversation, automatic tool execution loop, streaming callbacks.

- [x] **Refactor `stream_client_start_chat()`** — Now properly builds request body from client config + cJSON messages array using `build_request_body()` helper.

### Medium-term (Phase 2)

- [ ] **Integrate linenoise** — Replace `fgets()` in test programs with linenoise for:
  - Line editing and history
  - Prompt display
  - Multi-line input support

- [ ] **Multi-turn conversation in runtime test** — Runtime already supports this; the test program could be enhanced to show history inspection/management

### Medium-term (Phase 2)

- [ ] **Multi-turn conversation in test program** — Feed assistant responses back into LLM Parser history and allow follow-up questions with full context

- [ ] **Tool execution loop** — In runtime, detect tool calls from LLM, dispatch to registered tool handlers, feed results back, continue the conversation loop

- [ ] **Integrate hl-vt100** — For rich terminal output:
  - Syntax-highlighted streaming output (markdown/code blocks)
  - Progress indicators
  - Structured display for reasoning vs. content

### Future / Nice-to-have

- [ ] **Multiple concurrent streams** — Test and document multiple simultaneous SSE connections
- [ ] **Connection pooling / reuse** — Keep-alive support in curl
- [ ] **Error recovery** — Automatic retry on transient errors
- [ ] **Streaming abort improvements** — More graceful cancellation with proper cleanup
- [ ] **Automated tests** — Unit tests for parser, integration tests with mock server
- [ ] **libmill epoll/kqueue auto-detection** — Enable proper poller selection in CMake

---

## Build Instructions

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Environment

Copy `.env.example` or set:
```env
OPENAI_API_KEY=sk-xxx
OPENAI_BASE_URL=https://api.deepseek.com/v1
MODEL_NAME=deepseek-v4-flash
```
