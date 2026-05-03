# AGENT.md - Project Progress Summary

## Project: async_c_http

An **async HTTP SSE (Server-Sent Events) client in C** for streaming OpenAI-compatible API responses, using **libmill** coroutines and **libcurl** in forked child processes (`mfork`).

---

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

- **libcurl** runs in forked child process(es) via `mfork()` for blocking HTTP I/O
- **pipe** transfers data from child process to libmill coroutine in parent process
- **fdwait()** enables async waiting on pipe in libmill's event loop
- **Cancellation**: `kill(child_pid, SIGKILL)` terminates the child process immediately — no xfer callback needed

---

## Component Status

| Component | Files | Status | Description |
|---|---|---|---|
| **SSE Parser** | `openai_sse_parser.h/.c` | ✅ Stable | SSE event extraction, JSON validation, chunk parsing (content, reasoning, tool_calls, usage) |
| **Tool Call Parser** | `tool_call_parser.h/.c` | ✅ Stable | Streaming tool_call delta fragment accumulation |
| **Stream Client** | `openai_stream_client.h/.c` | ✅ Stable | Async streaming client (libmill coroutines + mfork child process + libcurl + pipe). Dynamic request body builder with tool schema support |
| **LLM Parser** | `llm_parser.h/.c` | ✅ Stable | Multi-turn conversation state machine. `force_finish()` for cancellation cleanup |
| **LLM Runtime** | `llm_runtime.h/.c` | ✅ Stable | High-level orchestration: wraps stream_client + llm_parser + tool execution + cancellation |
| **Runtime Test** | `llm_runtime_test.c` | ✅ Stable | Interactive REPL with isocline multiline input, tool demo, streaming callbacks |
| **Old Test** | `openai_stream_client_async_test.c` | ⚠️ Legacy | Still works but deprecated |
| **Utils** | `utils.c/.h` | ✅ Done | .env file loader |
| **Test Server** | `test_server.py` | ✅ Done | Python aiohttp SSE test server |
| **isocline** | `isocline/` | ✅ Integrated | Terminal input: multiline, history, BBCode colors |
| **hl-vt100** | `hl-vt100/` | ❌ Not used | Available for future rich terminal rendering |

---

## Detailed Breakdown

### 1. SSE Parser (`openai_sse_parser.h/.c`)
- `stream_buffer_t` — Ring-buffer for accumulating raw SSE data
- `StreamChunk` — Parsed struct: `content`, `reasoning_content`, `tool_calls`, `role`, `finish_reason`, `usage`
- `extract_next_chunk()` — Main parsing entry point

### 2. Tool Call Parser (`tool_call_parser.h/.c`)
- `ToolCallDeltaParser` — Up to 256 parallel tool call slots by index
- `feed_toolcall_delta()` — Accumulates incremental fragments across chunks
- Produces complete `cJSON` array on stream end

### 3. Stream Client (`openai_stream_client.h/.c`)
- `stream_client_new/free()` — Lifecycle. `free()` kills child via SIGKILL then `waitpid()` to reap.
- `stream_client_start_chat(client, messages)` — Accepts cJSON messages array, builds full request body, forks a child process via `mfork()`. Child runs `curl_easy_perform()`, writes HTTP stream to pipe, exits. Parent closes write end, reads from pipe via `fdwait()` in coroutine.
- `build_request_body()` — Static helper using cJSON API. Includes: model, messages, temperature, stream=true, tool schemas (if set)
- `stream_client_set_tool_schemas()` — Set tool definition JSON array (deep-copied, included in every subsequent request)
- `next_chunk()` / `next_chunk_nowait()` — Coroutine-based async chunk iteration. `extract_chunk_internal` loops on `c->running`, reads from pipe via `fdwait`. Pipe EOF (= child exit) breaks the loop naturally.
- `stream_client_cancel()` — Sets `running=0` + `kill(child_pid, SIGKILL)` to terminate the child process. Pipe EOF signals completion to the parent.
- `stream_client_wait_done()` — `waitpid()` to reap the child, checks exit status for error reporting.

### 4. LLM Parser (`llm_parser.h/.c`)
- State machine: `IDLE → IN_ASSISTANT → FINISHED`
- Status reporting: `REASONING`, `RESPONDING`, `WRITING_TOOL_CALL`, `FINISHED`
- `llm_parser_force_finish()` — On cancellation: finalizes partial content/reasoning, discards incomplete tool calls, keeps history valid

### 5. LLM Runtime (`llm_runtime.h/.c`)
High-level orchestration layer:

| API | Description |
|---|---|
| `llm_runtime_new/free()` | Lifecycle |
| `llm_runtime_send()` | **Coroutine**: sends message, streams via callback, auto tool loop |
| `llm_runtime_register_tool()` | Register C handler for a tool name |
| `llm_runtime_set_tool_schema()` | Set tool JSON schema for API request body |
| `llm_runtime_add_user_message()` | Add to history without sending |
| `llm_runtime_force_finish()` | Force-close partial assistant message |
| `llm_runtime_get_history()` | Read-only `{"messages":[...]}` |
| `llm_runtime_cancel()` | Cancel current turn |
| `llm_runtime_is_cancelled()` | Check cancellation (for tool functions) |

**Cancellation architecture**:
- `rt->running` is the single source of truth
- `llm_runtime_send()` resets `running=1` at start of each turn
- SIGINT → `llm_runtime_cancel()` → sets `running=0`
- Cancellation checks at every phase of send():
  - Before starting a stream → return
  - Mid-stream → kill child process (SIGKILL), force_finish parser, return
  - During tool execution → remaining tools get `{"error":"User has cancelled"}` result
- Tool functions receive `llm_runtime_t *rt` for cooperative cancellation

### 6. Runtime Test Program (`llm_runtime_test.c`)
- Global `g_rt` for SIGINT handler access
- **isocline** for input (replaced linenoise):
  - `ic_enable_multiline(true)` — **Enter submits**, **Shift+Enter / Ctrl+J inserts newline**
  - Persistent history via `ic_set_history(".history", 100)`
  - BBCode colored prompt: `[green][b]User[/] `
- Registered `sleep` tool demo with cancellation-aware stepping
- SIGINT: 1st press → cancel, 2nd press → exit

---

## Dependencies

| Library | Path | Purpose | Status |
|---|---|---|---|
| **libmill** | `libmill/` | Coroutines / async I/O | ✅ Integrated |
| **cjson** | `cjson/` | JSON parsing | ✅ Integrated |
| **isocline** | `isocline/` | Terminal input (multiline, history, colors) | ✅ Integrated |
| **hl-vt100** | `hl-vt100/` | VT100 rendering | ❌ Not yet used |

---

## Build

```bash
cd build && cmake .. && make -j$(nproc)
```

## Environment (`.env`)

```env
OPENAI_API_KEY=sk-xxx
OPENAI_BASE_URL=https://api.deepseek.com/v1
MODEL_NAME=deepseek-v4-flash
```

---

## Debug Log — mfork migration (2026-05-03)

### Bug 1: `fdwait` FDW_ERR / FDW_IN ordering (FIXED)

**Symptom**: First turn worked perfectly; second turn silently skipped the `finish_reason` chunk, leaving the LLM parser stuck in `IN_ASSISTANT` state. Third turn crashed with *"cannot add message while assistant stream in progress"*.

**Root cause**: When a child process exits, `close(pipe_write_end)` causes the parent's `fdwait()` on the read end to return **`FDW_ERR | FDW_IN`** (hangup + data-available). The original code checked `FDW_ERR` first and `break`-ed immediately — before the `FDW_IN` handler had a chance to `read()` the remaining data (finish_reason chunk + `[DONE]`) from the kernel pipe buffer. `stream_client_wait_done()` then closed the pipe, losing that data permanently.

Turn 1 worked by **timing luck**: the parent had already drained all data from the pipe before the child exited. Turn 2 had different timing and hit the race consistently.

**Fix**: Reorder — always drain `FDW_IN` first, then only `break` on `FDW_ERR` if `FDW_IN` was NOT also set:
```c
// BEFORE (buggy):
if (ev & FDW_ERR) { break; }         // breaks before reading
if (ev & FDW_IN) { read... }         // never reached

// AFTER (fixed):
if (ev & FDW_IN) { read... }         // always drain pipe first
if ((ev & FDW_ERR) && !(ev & FDW_IN)) { break; }  // real error only
```

**Lesson**: POSIX pipes signal EOF via `FDW_IN` + possibly `FDW_ERR`. Always prioritize draining data before acting on error flags. The libmill docs hint at this: *"If an error happens while there are still bytes to be received from the socket combination of FDW_ERR and FDW_IN may be returned."*

---

### Potential issue: `extract_next_chunk` returns 0 after consuming data

When `extract_next_chunk()` encounters `data: [DONE]`, cJSON rejects it as invalid JSON, so the function consumes `"data: "` (6 bytes) and returns `0` — leaving `[DONE]\n\n` in the buffer. The `[DONE]` is then correctly extracted on the **next** call via the `strncmp("[DONE]", 6)` check. This works, but it means one call to `extract_next_chunk` modifies buffer state while claiming "no chunk yet", forcing an extra `fdwait` round-trip. A cleaner approach would be to check for the `[DONE]` string immediately after consuming the `"data: "` prefix, avoiding the false-negative return.

---

### Potential issue: Byte counting across fork

After `mfork()`, `c->total_bytes` in the child process is a separate copy. The child's write callback increments its own `total_bytes`, but the parent never sees those increments. **Mitigated**: the parent now counts bytes on `read()` — `c->total_bytes += n` — giving an accurate count from the parent's perspective. The child's copy is irrelevant.

### Potential issue: Log interleaving across fork

Both parent and child share the same `c->log_fp` file descriptor (inherited across `mfork`). Concurrent writes from two processes to the same `FILE*` can interleave at the `stdio` buffer level. In practice this is benign because the child only logs during `curl_easy_perform()` (when the parent is waiting on `fdwait`), and the parent only logs before/after. But `fflush()` in one process does not flush the other process's `stdio` buffer. Acceptable for now; a future improvement could use `write()` directly or separate log files.

### Potential issue: Zombie processes on unclean shutdown

If `stream_client_wait_done()` is never called (e.g. the test program's `_exit(0)` in the SIGINT handler on second press), any running child process becomes a zombie. Currently `llm_runtime_send()` always calls `wait_done`, and `stream_client_free()` also calls `waitpid()`. But edge cases (e.g. double-SIGINT before the cleanup path runs) could leak zombies. A `SIGCHLD` handler or `waitpid(-1, &status, WNOHANG)` loop on shutdown would be more robust.

### Pre-existing issue (inherited from pthread version): Pipe buffer drain race

In the original pthread code, `c->curl_running = 0` was set by the curl thread **before** closing the pipe. The main loop condition `while (c->running && (c->curl_running || first_attempt))` would exit as soon as `curl_running` went to 0, even if data was still sitting in the kernel pipe buffer. This was masked because the write callback's `write()` blocked on pipe-full, providing backpressure that forced the main thread to keep reading. The new mfork code avoids this entirely by relying purely on pipe EOF (child closes write end → parent reads until `read()` returns 0).

### Pre-existing quirk: `[DONE]` marker extraction path

The `[DONE]` marker is only recognized after cJSON fails to parse it as JSON. The extraction path is:
1. Try cJSON → fails
2. Try event-boundary JSON extraction → fails
3. Consume `"data: "` prefix → return 0
4. Next call: `strncmp(buf->data, "[DONE]", 6)` matches → extract

This is fragile: any change to cJSON's behavior on `[DONE]` (e.g. treating it as a valid but empty array) would break the extraction. A dedicated check for `[DONE]` before attempting JSON parsing would be more robust.

### Architecture note: curl handle cleanup across fork

After `mfork()`, the parent calls `curl_easy_cleanup(curl)` on its copy of the curl handle while the child runs `curl_easy_perform()` on its own copy. This is safe because:
- `fork` + copy-on-write gives each process an independent copy of the curl handle
- `curl_easy_init()` allocates heap memory but does not open sockets
- The actual TCP connection is created by `curl_easy_perform()` in the child
- `curl_easy_cleanup()` in the parent frees only the parent's copy

The same applies to `curl_slist_free_all(headers)` and `free(body)` — both are safe to call in the parent immediately after fork.
