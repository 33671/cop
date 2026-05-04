# cop — async AI agent in C

A coroutine-based AI coding agent written in C. Uses **libmill** coroutines, **libcurl** in forked
child processes, and **SQLite** for persistent conversation history.

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
                 LLM API Server
```

- **libcurl** runs in `mfork()`-ed child processes for blocking HTTP I/O
- **pipe** transfers SSE stream from child to parent coroutine
- **fdwait()** enables async waiting in libmill's event loop
- **Cancellation**: `kill(pid, SIGKILL)` terminates child instantly

## Features

| Feature | Description |
|---------|-------------|
| Streaming SSE | Real-time token-by-token output with reasoning/content distinction |
| Tool execution | Shell, file read/write/edit tools with user approval and cancellation |
| Multi-turn | Automatic conversation history management with tool-call loops |
| Session history | SQLite persistence in `~/.agent/history.sql` |
| Model switching | `/set_model` at runtime, config loaded from `~/.agent/models.json` |
| CWD-scoped sessions | Sessions filtered by working directory |

## Dependencies

| Library | Purpose |
|---------|---------|
| [libmill](https://github.com/sustrik/libmill) | Coroutines / async I/O (bundled) |
| [libcurl](https://curl.se/) | HTTP client (system) |
| [SQLite3](https://sqlite.org/) | Conversation history (amalgamation in `sqlite/`) |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON parsing (bundled) |
| [isocline](https://github.com/daanx/isocline) | Terminal input: multiline, history, colors (bundled) |

## Quick Start

### 1. Configure models

Create `~/.agent/models.json`:

```json
{
  "providers": {
    "deepseek": {
      "baseUrl": "https://api.deepseek.com",
      "apiKey": "sk-your-key",
      "models": [
        {"id": "deepseek-v4-pro",   "contextWindow": 1000000},
        {"id": "deepseek-v4-flash", "contextWindow": 1000000}
      ]
    }
  }
}
```

### 2. Build

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
```

### 3. Run

```bash
./build/llm_runtime_test [--model <id>] [--log <file>]
```

### 4. REPL commands

| Command | Action |
|---------|--------|
| `<text>` | Send message, stream response |
| `/model` | List available models (`*` = current) |
| `/set_model <id>` | Switch model at runtime |
| `/sessions` | List CWD sessions with last message preview |
| `/load <id>` | Load a session (prints last 50 messages) |
| `/delete <id>` | Delete a session |
| `Ctrl+C` | Cancel current turn (twice to exit) |

## Project Structure

```
cop/
├── llm_runtime.c/h        High-level orchestration
├── openai_stream_client.c/h  Async SSE streaming client
├── openai_sse_parser.c/h   SSE event & chunk parser
├── llm_parser.c/h          Multi-turn state machine
├── tool_call_parser.c/h    Streaming tool-call accumulator
├── tool_functions.c/h      Built-in tools (shell, read, write, edit)
├── history_db.c/h          SQLite conversation storage
├── models_config.c/h       models.json parser
├── utils.c/h               .env loader
├── llm_runtime_test.c      Interactive REPL
├── sqlite/sqlite3.c/h      SQLite3 amalgamation
├── cjson/                  cJSON (bundled)
├── isocline/               Terminal input (bundled)
├── libmill/                Coroutine library (bundled)
├── CMakeLists.txt
└── test_server.py          SSE test server
```

## Architecture Details

### SSE Stream → Chunks → Parser → Runtime

```
Child (curl) → pipe → extract_next_chunk() → StreamChunk
    → llm_parser_feed_chunk() → content / reasoning / tool_calls
    → execute_tool_calls() → tool_fn(rt, args)
    → loop back to send another request if tools were called
```

### Tool Result Format

Tools return `{"type":"text", "text":"..."}` or `{"type":"image_url", "image_url":{...}}`.
Results are previewed inline on completion (first 200 chars / 3 lines).

### Cancellation

- `llm_runtime_cancel()` sets `running=0`
- Mid-stream: kills child process via SIGKILL, force-finishes parser
- Mid-tool: remaining tools get `{"error":"User has cancelled"}`
- Tool functions check `llm_runtime_is_cancelled(rt)` cooperatively

### History DB Schema

```sql
sessions(id, cwd, name, created_at)
messages(id, session_id, msg_index, role, content, reasoning_content,
         tool_call_id, tool_calls, created_at)
```

Sessions are created lazily on first message save. CWD scoping means different
project directories have independent session lists.
