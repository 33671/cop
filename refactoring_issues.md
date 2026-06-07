# 代码重构建议：高优先级结构性问题

> 本文档汇总了项目中需要优先处理的结构性重构问题，涵盖全局状态、函数过大、UI耦合、宏重复、进程管理及并发安全等方面。

---

## 1. 全局变量滥用 (Global State Pollution)

**影响文件**: `cop_ui.h` / `cop.c` / `llm_runtime.c`

**现状**: 项目通过 7 个全局变量在多个文件间共享核心状态：

```c
// cop_ui.h
extern llm_runtime_t        *g_rt;
extern history_db_t         *g_db;
extern int64_t               g_session_id;
extern int                   g_saved_count;
extern char                  g_cwd[4096];
extern model_entry_t       **g_models;
extern volatile sig_atomic_t g_want_exit;
extern volatile pid_t        g_running_child_pid;
```

**问题**:
- 无法同时运行多个 session（单例硬编码）
- 单元测试几乎不可能（无法注入 mock）
- Signal handler (`cop_ui_sigint`) 直接读写 `g_rt` 和 `g_running_child_pid`，存在隐式时序依赖
- 任何新增的跨模块数据都需要再加一个 `extern` 声明，持续恶化

**重构建议**:

```c
// 引入顶层上下文结构体
typedef struct cop_context {
    llm_runtime_t        *rt;
    history_db_t         *db;
    int64_t               session_id;
    int                   saved_count;
    char                  cwd[4096];
    model_entry_t       **models;
    volatile sig_atomic_t want_exit;
    volatile pid_t        running_child_pid;
} cop_context_t;

// cop_ui.h 改为
void cop_ui_init(cop_context_t *ctx);
coroutine void cop_ui_repl(cop_context_t *ctx);
void cop_ui_sigint(int sig, siginfo_t *info, void *uap);
// g_rt 等 extern 全部删除
```

**迁移步骤**:
1. 创建 `cop_context_t`，将现有全局变量迁入
2. 修改 `cop_ui_init` / `cop_ui_repl` / `cop_ui_sigint` 接受 `cop_context_t *`
3. `main()` 中分配 `cop_context_t ctx = {0}`，传递指针
4. Signal handler 通过 `uap` 或 `sigaction` 的 `sa_sigaction` 参数传入上下文（或使用 `timer_t` / `signalfd` 替代）

---

## 6. `llm_runtime_send` 函数过大 (God Function)

**影响文件**: `llm_runtime.c` → 提取为 `agent_loop.c` / `agent_loop.h`

### 6.1 现状

`llm_runtime_send` 从第 422 行到第 746 行，**超过 300 行**，承担了过多职责：

```c
coroutine int llm_runtime_send(llm_runtime_t *rt,
                               const char *user_text,
                               llm_runtime_callback_t on_chunk,
                               void *user_data) {
    // 1. 重置 cancellation flag (10 行)
    // 2. 添加 user message 到 history (30 行)
    // 3. 主 tool loop (200+ 行)
    //    - 获取 messages
    //    - 发起 HTTP 请求
    //    - 处理 chunk 流
    //    - 通知 status change
    //    - 收集 usage
    // 4. 检测 tool calls (50 行)
    // 5. 执行 tool calls (调用 execute_tool_calls)
    // 6. 错误处理与重试 (30 行)
    // 7. 最终清理 (20 行)
}
```

**问题**:
- 单函数认知负荷过高，难以阅读和维护
- 错误处理路径分散在 6 个 `return -1` 和多个 `goto` 式的逻辑分支中
- 难以单独测试"chunk 处理"或"tool 执行"逻辑
- `llm_runtime.c` 既暴露了公开 API，又包含了最重的 agent loop 实现，文件职责不纯

### 6.2 重构方案：提取 `agent_loop.c` / `agent_loop.h`

**设计原则**:
- `agent_loop` 是一个**内部模块**，不对外暴露
- 只包含 `llm_runtime_send` 的完整实现体
- `llm_runtime.c` 保留公开 API 的 thin wrapper（lifecycle、配置、历史访问等）

**目标文件结构**:

```
.
├── llm_runtime.h          # 公开 API（保持不变）
├── llm_runtime.c          # 公开 API 实现（变薄）
│   ├── llm_runtime_new()
│   ├── llm_runtime_free()
│   ├── llm_runtime_set_model()
│   ├── llm_runtime_register_tool()
│   ├── llm_runtime_add_user_message()
│   └── llm_runtime_send()   # 改为转发到 agent_loop_run()
│
├── agent_loop.h            # 内部头文件（不安装）
│   └── agent_loop_run()
│
└── agent_loop.c            # agent loop 完整实现（从 llm_runtime.c 迁入）
    ├── agent_loop_run()                # 原 llm_runtime_send 主体
    ├── execute_tool_calls()            # 已有 static，改为文件内 static 或导出
    ├── add_tool_result_to_history()    # 已有 static
    └── 相关辅助函数
```

**`agent_loop.h` 设计**（内部头文件，不对外暴露）:

```c
#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "llm_runtime.h"

/*
 * agent_loop_run — execute one complete agent turn.
 *
 * This is the actual implementation formerly in llm_runtime_send().
 * It is called by the public llm_runtime_send() wrapper.
 *
 * Returns 0 on success, -1 on error.
 */
int agent_loop_run(llm_runtime_t *rt,
                   const char *user_text,
                   llm_runtime_callback_t on_chunk,
                   void *user_data);

#endif /* AGENT_LOOP_H */
```

**`llm_runtime.c` 中的 wrapper**:

```c
#include "agent_loop.h"  // 内部头文件

// 公开 API — 薄转发层
coroutine int llm_runtime_send(llm_runtime_t *rt,
                               const char *user_text,
                               llm_runtime_callback_t on_chunk,
                               void *user_data) {
    return agent_loop_run(rt, user_text, on_chunk, user_data);
}
```

**迁移步骤**:
1. 创建 `agent_loop.h` / `agent_loop.c`
2. 将 `llm_runtime_send` 整体复制到 `agent_loop.c`，改名为 `agent_loop_run`
3. 将 `execute_tool_calls`、`add_tool_result_to_history` 等 static helper 一并迁入（保持 static 或按需改为文件内共享）
4. `llm_runtime.c` 中只保留 thin wrapper，`#include "agent_loop.h"`
5. 更新 `CMakeLists.txt`，将 `agent_loop.c` 加入编译单元
6. 清理 `llm_runtime.c` 中不再需要的 `#include`（如 `sys/wait.h`、`signal.h` 如果不再直接使用）

### 6.3 后续优化（可选，第二阶段）

待 `agent_loop.c` 独立后，可继续拆分子模块：

```
agent_loop.c (当前提取后的状态，约 350 行)
├── agent_loop_run()           # 主循环 + tool loop
├── execute_tool_calls()       # tool 执行
├── add_tool_result_to_history()
│
├── agent_stream.c             # 第二阶段提取
│   └── agent_stream_once()    # 单次 HTTP 请求 + chunk 处理
│
└── agent_tools.c              # 第二阶段提取
    ├── agent_detect_tools()   # 从 parser 提取 tool_calls
    └── agent_execute_tools()  # 执行 + 结果回写
```

但第一阶段（提取 `agent_loop.c`）已经能显著降低 `llm_runtime.c` 的认知负荷，建议先到此为止，验证稳定后再考虑第二阶段。

---

## 8. Tool 审批逻辑耦合了 isocline UI

**影响文件**: `tool_functions.c`

**现状**: `ask_approval()` 直接调用 isocline 库：

```c
static int ask_approval(llm_runtime_t *rt, const char *prompt) {
    (void)rt;
    if (llm_runtime_is_yolo(rt)) return 1;
    ic_enable_multiline(false);  // 直接操作 UI 库
    char *reply = ic_readline(prompt);
    ic_enable_multiline(true);
    ...
}
```

**问题**:
- `tool_functions` 模块本应只关心工具业务逻辑，却依赖了 `isocline/include/isocline.h`
- 如果未来要支持 Web UI、GUI、或非交互模式（如 batch 脚本），所有 tool 函数都需要修改
- `ic_enable_multiline` 的开关逻辑（单行输入 → Enter 提交）散落在每个需要审批的工具里（`tool_shell`, `tool_write`, `tool_edit`）

**重构建议**: 引入审批回调接口

```c
// tool_functions.h
typedef int (*tool_approval_cb_t)(const char *tool_name,
                                  const char *args_json,
                                  void *user_data);

typedef struct tool_execution_context {
    llm_runtime_t *rt;
    tool_approval_cb_t approval_cb;
    void *user_data;
} tool_execution_context_t;

// 修改所有 tool 函数签名
cJSON *tool_shell(llm_runtime_t *rt, const cJSON *args);
// ↓
cJSON *tool_shell(const tool_execution_context_t *ctx, const cJSON *args);

// 内部审批调用变为
static int check_approval(const tool_execution_context_t *ctx,
                          const char *prompt,
                          const char *deny_msg) {
    if (ctx->approval_cb) {
        return ctx->approval_cb(prompt, deny_msg, ctx->user_data);
    }
    // fallback: 默认允许（用于非交互模式）
    return 1;
}
```

**在 `cop_ui.c` 中注册具体实现**:
```c
static int ui_approval_cb(const char *prompt, const char *deny_msg, void *user) {
    (void)deny_msg;
    ic_enable_multiline(false);
    char *reply = ic_readline(prompt);
    ic_enable_multiline(true);
    // ... 原有逻辑
}

// 注册时
tool_execution_context_t ctx = {
    .rt = rt,
    .approval_cb = ui_approval_cb,
    .user_data = NULL
};
```

**迁移步骤**:
1. 定义 `tool_execution_context_t` 结构体
2. 修改 `tool_functions.h` 中所有 tool 函数签名（或加一层 wrapper）
3. 将 `ask_approval` 改为接受 context，不再直接 include isocline
4. 在 `cop_ui.c` 中实现具体的 isocline-based approval callback
5. 通过 `llm_runtime_set_tool_context()` 或类似机制注入

---

## 9. 重复的 debug_log 宏定义

**影响文件**: `llm_runtime.c`, `cop_ui.c`, `tool_functions.c` 等多个文件

**现状**: 每个 `.c` 文件顶部都重复定义：

```c
/* [debug] log — compiled out unless -DDEBUG is set */
#ifdef DEBUG
#define debug_log(...)  fprintf(stderr, __VA_ARGS__)
#else
#define debug_log(...)  ((void)0)
#endif
```

**问题**:
- 违反 DRY 原则
- 如果未来想改变 debug 输出格式（如加时间戳、文件:行号），需要修改所有文件
- 不同文件可能悄悄不一致（比如有的加了 `\n`，有的没加）

**重构建议**: 统一放到 `debug.h`

```c
// debug.h
#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#ifdef DEBUG
    #define debug_log(fmt, ...) \
        fprintf(stderr, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define debug_log(fmt, ...) ((void)0)
#endif

#endif // DEBUG_H
```

**使用**:
```c
// 任何 .c 文件
#include "debug.h"

debug_log("tool[%d/%d]: %s id=%s\n", i+1, tc_count, name, call_id);
```

**进阶优化**: 如果想支持运行时日志级别（而不是编译期开关），可以改用函数调用：

```c
// debug.c
void debug_log_impl(const char *file, int line, const char *fmt, ...) {
    if (!debug_enabled()) return;
    fprintf(stderr, "[%s:%d] ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// debug.h
#ifdef DEBUG
    #define debug_log(fmt, ...) \
        debug_log_impl(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
    #define debug_log(fmt, ...) ((void)0)
#endif
```

---

## 10. openai_stream_client.c 中 Child Process 管理复杂

**影响文件**: `openai_stream_client.c` (600+ 行)

**现状**: 单个文件包含了以下所有逻辑：

```
openai_stream_client.c (600+ 行)
├── 日志函数 (get_timestamp_ms, get_timestamp_str, log_write, log_raw_data)
├── CURL Write Callback (client_curl_write_cb)
├── Client Lifecycle (stream_client_new, stream_client_free)
├── 配置 Setter (set_system_message, set_temperature, set_model, set_api, ...)
├── Request Body Builder (build_request_body)
├── Child Process 管理 (fork, exec curl, pipe, waitpid)
├── SSE 流解析循环 (next_chunk, buffer_looks_like_sse)
├── 重试逻辑 (CLIENT_STATE_CONNECTING -> CLIENT_STATE_STREAMING)
└── 错误分类 (CHILD_EXIT_OK, CHILD_EXIT_NET_ERR, ...)
```

**问题**:
- **文件过大**: 超过 600 行，单一职责 violated
- **关注点混杂**: 网络协议、进程管理、日志、配置、重试策略全搅在一起
- **测试难度**: 无法单独测试"重试逻辑"而不触发真实的 fork/curl
- **错误处理不一致**: exit code 常量 (`CHILD_EXIT_OK` 等) 与 `client_state_t` 枚举职责重叠

**重构建议**: 拆分为 3 个模块

```
stream_client/
├── stream_client.h          # 对外 API（保持不变）
├── stream_client.c          # 生命周期、配置、公开入口（thin wrapper）
├── stream_curl.c            # curl 子进程管理
│   ├── curl_child_main()    # 子进程入口：setup curl, write to pipe
│   ├── curl_parent_reader() # 父进程：fdwait + pipe_drain
│   └── curl_retry_policy()  # 重试逻辑（指数退避、错误分类）
├── stream_sse.c             # SSE 协议解析
│   ├── sse_buffer_append()
│   ├── sse_parse_events()
│   └── buffer_looks_like_sse()
└── stream_log.c             # 日志（可选，或保留在 utils）
```

**关键提取点**:

```c
// stream_curl.h
typedef enum {
    CURL_CHILD_OK = 0,
    CURL_CHILD_NET_ERR,      // retryable
    CURL_CHILD_HTTP_4XX,     // not retryable
    CURL_CHILD_HTTP_5XX,     // retryable
    CURL_CHILD_HTTP_429,     // retryable (rate limit)
} curl_child_exit_t;

// 启动子进程，返回 exit code 管道
curl_child_exit_t curl_child_start(stream_client_t *c,
                                   const char *request_body,
                                   pid_t *out_pid);

// 等待子进程结束，返回最终状态
int curl_child_wait(stream_client_t *c, pid_t pid, int *out_exit_code);
```

**迁移步骤**:
1. 将 `client_curl_write_cb` 和 `CHILD_EXIT_*` 常量移到 `stream_curl.c`
2. 将 `build_request_body` 和配置 setter 移到 `stream_client.c`（保持现有 API 不变）
3. 将 `next_chunk`、`stream_buffer_*`、`buffer_looks_like_sse` 移到 `stream_sse.c`
4. 重试逻辑提取为 `curl_should_retry(exit_code, attempt)` 函数
5. `stream_client.c` 变成薄胶水层，组合上述模块

---

## 12. `mkdir_p` 有竞态条件与错误忽略

**影响文件**: `tool_functions.c`

**现状**:

```c
static int mkdir_p(const char *path) {
    if (strlen(path) >= 1024) return -1;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    return 0;
}
```

**问题**:

1. **TOCTOU 竞态条件** (Time-of-check to Time-of-use):
   - `mkdir(tmp, 0755)` 在 `tmp` 已存在时会返回 -1 (EEXIST)
   - 当前代码完全忽略 `mkdir` 返回值，表面上"能工作"，但掩盖了真实错误
   - 在多进程/多 coroutine 环境下（虽然 libmill 是单线程，但 tool 里会 `fork`），另一个进程可能在同一时刻创建该目录

2. **栈缓冲区溢出风险**:
   - `char tmp[1024]` 在栈上，`path` 如果恰好 1023 字节长，`snprintf` 会截断，导致后续循环在错误位置切割路径

3. **权限硬编码**:
   - `0755` 硬编码，无法配置（虽然对于这个场景可能无所谓）

**重构建议**:

```c
#include <errno.h>
#include <sys/stat.h>

static int mkdir_p(const char *path) {
    if (!path || path[0] == '\0') return -1;
    
    // 使用堆缓冲区，避免栈溢出风险
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, len + 1);
    
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            // 忽略 EEXIST，但报告其他错误
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    
    // 处理最后一层目录
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    
    free(tmp);
    return 0;
}
```

**更优方案** (POSIX.1-2008 可用时):

```c
#ifdef __GLIBC__
    // GNU/Linux 有 dirname 可以配合
#else
    // 手动画分步创建
#endif

// 或者直接使用 mkdtemp + rename  trick（如果需要原子性）
```

**迁移步骤**:
1. 将 `mkdir_p` 改为堆分配，修复潜在溢出
2. 正确处理 `EEXIST`，其他错误返回 -1 并设置 `errno`
3. 在 `tool_write` / `tool_edit` 调用处检查返回值并给出友好错误信息

---

## 总结

| 问题 | 影响范围 | 重构难度 | 优先级 |
|------|---------|---------|--------|
| 1. 全局变量污染 | 架构级 | 高（需全链路修改） | P0 |
| 6. llm_runtime_send 过大 | 单模块 | 中（机械拆分） | P1 |
| 8. Tool 审批耦合 UI | 模块边界 | 中（需改签名） | P1 |
| 9. debug_log 重复 | 工程规范 | 低（纯文本替换） | P2 |
| 10. Child Process 管理复杂 | 单模块 | 高（需拆分文件） | P1 |
| 12. mkdir_p 竞态条件 | 局部 | 低（函数内修改） | P2 |

**建议执行顺序**:
1. 先处理 **问题 9**（debug_log）— 零风险，快速 win
2. 并行处理 **问题 12**（mkdir_p）— 修复潜在 bug
3. 处理 **问题 1**（全局变量）— 架构地基，必须先于其他模块化工作
4. 处理 **问题 6**（拆分 llm_runtime_send）— 降低后续重构成本
5. 处理 **问题 8**（Tool Approval 解耦）— 为多 UI 支持铺路
6. 最后处理 **问题 10**（Child Process 拆分）— 工作量最大，可逐步迁移

---

*文档生成时间: 2025-06-07*
