# Async C HTTP Agent — Implementation Plan

## Goal
Evolve the current C-based agent runtime into a cheaper, safer, and more maintainable system without rewriting the core loop.

## Existing Code Areas
- `agent_loop.c/h` — turn orchestration and tool loop
- `llm_runtime.c/h` — runtime state, cancellation, error handling
- `openai_stream_client.c/h` — streaming HTTP client
- `llm_parser.c/h` — message/history parsing and stream state
- `tool_functions.c/h` — tool registry and tool implementations
- `history_db.c/h` — persistent conversation storage
- `models_config.c/h` — model and endpoint configuration
- `stream_*` and parser modules — output handling and SSE parsing

## Primary Design Constraints
- Keep the system stateless at the API boundary.
- Keep conversation state owned by the application.
- Avoid repeatedly sending large raw payloads when references will do.
- Preserve the existing streaming + tool-call architecture.
- Make each improvement shippable in small steps.

## Phase 1 — History Windowing and Summarization

### Objective
Reduce prompt size and network traffic by sending only the relevant context.

### Implementation Tasks
1. Add a history policy struct to runtime/config:
   - `max_messages`
   - `max_user_turns`
   - `max_tool_messages`
   - `summary_trigger_messages`
   - `summary_trigger_tokens` if token estimates are available

2. Add a history pruning function, e.g.:
   - `llm_parser_prune_history(rt->parser, policy)`
   - keep system prompt + recent turns
   - collapse old turns into one summary message

3. Add a summary generation path:
   - when history exceeds threshold, create a summary request
   - store the summary as a dedicated system/developer memory message
   - preserve facts, user preferences, open tasks, and tool outputs

4. Add a summary message format, for example:
   - `role: system`
   - `content: "Conversation summary: ..."`

5. Make pruning happen before `stream_client_start_chat()` is called in `agent_loop_run()`.

### Files Likely to Change
- `agent_loop.c`
- `llm_parser.c/h`
- `llm_runtime.c/h`
- `history_db.c/h`
- `models_config.c/h`

### Acceptance Criteria
- Long chats no longer grow without bound.
- Recent turns remain intact.
- Older context is compressed into summaries.
- No visible breakage in tool-call behavior.

---

## Phase 2 — Large Artifact Handling

### Objective
Stop repeatedly sending large base64 blobs or embedded file payloads in conversation history.

### Implementation Tasks
1. Define an artifact abstraction:
   - `id`
   - `type` (`image/png`, `text/plain`, etc.)
   - `size_bytes`
   - `path` or `uri`
   - optional `sha256`

2. Add an artifact store module:
   - save tool-produced files to disk
   - return stable references instead of raw content
   - support retrieval by ID

3. Change tool result format for large outputs:
   - instead of embedding base64 in `content`, store it externally
   - put a small JSON reference in the message history

4. Add size thresholds:
   - if output exceeds threshold, store externally
   - if output is tiny, inline it

5. Update tool result handling in `agent_loop.c`:
   - convert large `image_url` or blob outputs into artifact refs before history insertion
   - ensure previews stay short

### Files Likely to Change
- `agent_loop.c`
- `tool_functions.c/h`
- new `artifact_store.c/h`
- `history_db.c/h`
- `llm_parser.c/h`

### Acceptance Criteria
- Base64 data is no longer repeatedly copied into the prompt.
- Images/files are referenced by stable IDs or URLs.
- Tool outputs remain readable without exploding payload size.

---

## Phase 3 — Tool Execution Hardening

### Objective
Make tool calling safer, more deterministic, and easier to debug.

### Implementation Tasks
1. Add a tool registry entry format with metadata:
   - tool name
   - function pointer
   - timeout ms
   - concurrency-safe flag
   - input/output schema pointers if available

2. Add per-tool timeout support:
   - enforce a max execution time
   - if a tool exceeds timeout, return structured error output

3. Add JSON schema validation or lightweight field validation:
   - validate required keys before calling the tool
   - validate tool output shape before adding to history

4. Improve error handling paths:
   - invalid JSON arguments
   - missing tool name/id
   - unknown tool
   - malformed tool output

5. Add optional parallel tool execution:
   - allow independent tools to run concurrently
   - preserve result ordering when writing back to history

6. Record timing and outcomes:
   - start/end timestamps
   - success/failure/timeout counts

### Files Likely to Change
- `tool_functions.c/h`
- `agent_loop.c`
- `llm_runtime.c/h`
- new `tool_runtime.c/h` if the tool subsystem grows

### Acceptance Criteria
- Tool failures do not corrupt the agent state.
- Timeouts are enforced.
- Invalid arguments are reported clearly.
- Parallel execution can be enabled without breaking ordering.

---

## Phase 4 — Persistent Memory Layer

### Objective
Separate short-term chat context from durable memory.

### Implementation Tasks
1. Define memory categories:
   - user preferences
   - stable facts
   - project state
   - session summaries
   - tool-derived knowledge

2. Extend `history_db` or create `memory_db` to store:
   - timestamp
   - source turn
   - memory type
   - text payload
   - optional relevance score

3. Add memory retrieval before each request:
   - fetch only relevant memory items
   - inject them into prompt as a compact context block

4. Add summary refresh logic:
   - if a session is old, keep a compact summary and archive raw turns
   - avoid loading everything every time

5. Add explicit memory update hooks:
   - when user says a preference, store it
   - when a task completes, store the outcome

### Files Likely to Change
- `history_db.c/h`
- new `memory_db.c/h`
- `agent_loop.c`
- `llm_runtime.c/h`

### Acceptance Criteria
- The agent can remember useful facts without bloating the prompt.
- Short-term and long-term state are clearly separated.
- Memory injection is controllable and explainable.

---

## Phase 5 — Reliability and Observability

### Objective
Make failures visible and debugging easier.

### Implementation Tasks
1. Add structured logging levels:
   - debug
   - info
   - warning
   - error

2. Log key lifecycle events:
   - request start/end
   - stream open/close
   - parser state changes
   - tool start/end
   - cancellation
   - retries

3. Add counters/metrics:
   - request latency
   - tokens in/out
   - tool calls per turn
   - retry count
   - cancellation count
   - stream errors

4. Add retry policy for transient network failures:
   - backoff delay
   - max retry count
   - only retry safe failure types

5. Add tests for failure scenarios:
   - invalid tool args
   - canceled stream
   - parser error
   - mid-stream network failure
   - tool timeout

### Files Likely to Change
- `debug.h`
- `llm_runtime.c/h`
- `openai_stream_client.c/h`
- `agent_loop.c`
- test files under `*_test.c`

### Acceptance Criteria
- Errors are visible and actionable.
- The runtime can recover from common transient failures.
- Regression tests cover the critical paths.

---

## Phase 6 — Module Boundaries and Cleanup

### Objective
Reduce coupling and make future changes easier.

### Implementation Tasks
1. Split responsibilities more cleanly:
   - transport
   - parsing
   - agent orchestration
   - tool execution
   - persistence
   - metrics/logging

2. Move helper logic out of `agent_loop.c` where appropriate.

3. Introduce a configuration struct that covers:
   - model name
   - max tool loops
   - history limits
   - summary settings
   - artifact thresholds
   - retry policy

4. Make `agent_loop_run()` easier to unit test by:
   - isolating side effects
   - injecting dependencies where possible

### Files Likely to Change
- `agent_loop.c/h`
- `llm_runtime.c/h`
- `models_config.c/h`
- new `config.c/h`
- `tool_functions.c/h`

### Acceptance Criteria
- New features do not require editing the entire loop.
- The code is easier to read and test.
- Configuration is centralized.

---

## Suggested Build Order
1. Add history windowing and summaries.
2. Add artifact references for large outputs.
3. Add tool timeouts and validation.
4. Add persistent memory retrieval.
5. Add structured logging and tests.
6. Refactor module boundaries.

## Minimal First Milestone
If time is limited, implement just this:
- prune old history
- summarize old turns
- replace large base64 payloads with artifact references

That alone will reduce cost and make the agent much easier to use.

## Definition of Done
The project is in a much better state when:
- long conversations stay within a bounded context size
- tool outputs do not repeatedly bloat history
- large files and images are referenced instead of copied
- tool calls have timeouts and validation
- memory is split into short-term and long-term layers
- logs and tests make regressions easy to catch

## Guiding Rule
Prefer incremental implementation over redesign. Keep the agent loop intact, and improve the surrounding systems one piece at a time.
