# TODO

## Arena / cJSON 内存管理

当前 `cjson_arena.c` 将 cJSON 的全局分配器 hook 到 arena，`cJSON_Delete`/`cJSON_free` 变成空操作，导致整个会话期间 cJSON 分配的内存只增不减。Arena (`arena.h`) 已提供 snapshot/rewind 机制，只需接入。

### ✅ 已完成：per-step `arena_trim()`

见 `llm_runtime.c` `llm_runtime_send()` 中的 trim 调用点。

- `llm_parser_trim()` → 在每个 tool-loop step 结束后释放 parser arena 的额外 regions
- `tool_arena_trim()` → 同理，释放 tool function 共享 arena 的额外 regions
- `cjson_arena_trim()` → `cjson_arena.h` 已声明，`cjson_arena.c` 已实现，但暂未在 step 路径中调用（因 persistent cJSON 历史节点与临时节点混在同一 arena 中）

TODO 中关于 snapshot/rewind 的规划仍然有效，`arena_trim()` 是一个更轻量的中间步骤。

### 1. 暴露 arena snapshot/rewind 接口

- [ ] `cjson_arena.h`：声明 `cjson_arena_snapshot()` 和 `cjson_arena_rewind(Arena_Mark)`，外部可调用
- [ ] `cjson_arena.c`：实现上述两个函数，封装对全局 `cjson_arena` 的操作

### 2. 梳理 cJSON 对象的生命周期

需要区分两类 cJSON 对象：

| 生命周期 | 示例 | 处理方式 |
|---|---|---|
| **请求内临时** | SSE 解析中间结果、tool call 参数解析、构建请求 body | rewind 回收 |
| **跨请求持久** | `llm_runtime` 内部的对话历史 (`messages` 数组)、tool schema | 需在 snapshot 之前已存在于 arena，或用堆内存独立存储 |

具体需要确认的文件和对象：

- [ ] `llm_runtime.c`：`rt->parser` 中的 history/messages 是跨轮次的，确认存储方式
- [ ] `openai_stream_client.c`：每次请求的临时 cJSON 节点
- [ ] `openai_sse_parser.c`：SSE 增量解析产生的临时节点
- [ ] `tool_call_parser.c`：tool call 参数 JSON 解析
- [ ] `tool_functions.c`：`tool_functions_create_schema()` 只在初始化调用一次 (`cop.c:128`)，之后 `cJSON_Delete`，需确认 schema 是否被 runtime 内部引用
- [ ] `history_db.c`：`history_db_load_session()` / `history_db_list_sessions()` 返回的 cJSON 在 `cop_ui.c` 中使用后立即 `cJSON_Delete`，属于临时对象
- [ ] `cop_ui.c`：渲染时读取 cJSON 但不持有，临时对象

### 3. 确定 snapshot/rewind 的调用位置

- [ ] 每个对话轮次的边界：在 `llm_runtime` 处理用户输入之前打 snapshot，本轮所有工具调用完成后 rewind
- [ ] 或者在 `cop_ui_repl` 的主循环中：读用户输入前 snapshot，LLM 响应 + 工具调用全部完成后 rewind

### 4. 处理跨轮次对象

- [ ] 如果 runtime 的 history messages 是 cJSON 节点且存在 arena 中：需在 snapshot 之前用 `cJSON_Duplicate` 到堆内存，或改用独立 arena
- [ ] 或者：将 history 存储改为非 cJSON 形式（如纯文本序列化到 SQLite，本身已经在 `history_db` 中有存储）

### 5. 验证

- [ ] 用 heaptrack 对比修复前后的内存曲线，确认不再单调增长
- [ ] 跑多轮对话测试，确认对话历史没有丢失或损坏
- [ ] 确认 rewind 后旧指针不会被误用（arena 不清零内容，use-after-rewind 是无声的 bug）
