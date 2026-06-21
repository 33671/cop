#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["aiohttp"]
# ///
"""
OpenAI API format dummy data server.

Simulates an OpenAI-compatible chat completions endpoint that returns
randomly generated reasoning, content, and executable shell tool calls.

Usage:
    uv run test_server.py [port]

Default port is 5344.
"""

import asyncio
import sys
import random
import json
import uuid
import time
from datetime import datetime
from aiohttp import web

# ── Random shell commands that can actually be executed ──────────────────────

SHELL_COMMANDS = [
    # File system inspection
    "ls -la",
    "pwd",
    "ls -la /tmp",
    "ls -la .",
    "ls -la src/",
    "ls -la include/",
    # Status / info
    "date",
    "whoami",
    "uname -a",
    "id",
    "uptime",
    "hostname",
    "df -h",
    "free -h",
    "lscpu 2>/dev/null || sysctl -n hw.ncpu",
    # Git
    "git status",
    "git log --oneline -5",
    "git diff --stat",
    "git branch -a",
    # File content
    "cat Makefile",
    "cat CMakeLists.txt",
    "cat README.md",
    "cat .gitignore",
    "cat src/main.c 2>/dev/null | head -30",
    "cat include/tool_functions.h 2>/dev/null | head -50",
    "cat tool_functions.c 2>/dev/null | head -60",
    # Counting
    "wc -l src/*.c 2>/dev/null | tail -5",
    "wc -l include/*.h 2>/dev/null | tail -5",
    "du -sh * 2>/dev/null",
]

READ_PATHS = [
    "Makefile",
    "CMakeLists.txt",
    "README.md",
    ".gitignore",
    "src/main.c",
    "src/tool_functions.c",
    "include/tool_functions.h",
    "include/diff.h",
]

# ── Random content generators ────────────────────────────────────────────────

REASONING_WORDS = [
    # Basic thinking fragments
    "好的", "让我", "先", "看一下", "当前", "项目", "的", "文件", "结构",
    "我需要", "检查", "相关", "源代码", "理解", "实现", "细节", "再做", "修改",
    "让我先", "查看", "一下", "项目状态", "和", "内容",
    "我来", "分析", "这个", "问题", "看看", "代码", "是", "怎么", "实现的",
    "先检查一下", "构建", "系统", "整体", "架构",
    "查看", "源文件", "当前",
    "这个", "需要", "了解", "当前代码", "实现",
    "先查看", "项目结构", "关键", "文件",
    # Extended reasoning fragments for longer thinking
    "接下来", "然后", "还需要", "进一步", "深入", "研究", "该模块",
    "看起来", "可能", "存在问题", "需要修复", "建议", "修改方案",
    "对比", "不同", "实现方式", "权衡", "利弊", "选择", "最佳",
    "参考", "文档", "设计模式", "最佳实践", "性能", "安全",
    "考虑到", "边界条件", "错误处理", "兼容性", "可维护性",
    "单元测试", "集成测试", "覆盖率", "回归测试",
    "重构", "优化", "抽象", "封装", "接口", "依赖",
    "分析结果", "表明", "核心问题", "在于", "需要调整",
    "检查了", "相关调用", "以及", "上下游", "影响",
    "同时", "注意", "还要", "额外", "处理", "特殊情况",
    "验证", "确认", "确保", "避免", "潜在", "风险",
    "根据经验", "常见", "做法", "通常", "推荐", "采用",
    "评估", "复杂度", "可读性", "扩展性", "健壮性",
    "逐步", "排查", "定位", "根因", "解决", "方案",
    "总结", "归纳", "得出", "结论", "下一步", "计划",
    "备注", "补充", "说明", "细节", "注意事项",
]

CONTENT_WORDS = [
    # Basic response fragments
    "好的", "我来", "看看", "相关", "文件",
    "让我", "检查一下", "这个", "项目的", "结构",
    "根据", "我的", "分析", "问题", "出在", "这里",
    "我查看了", "一下", "代码", "情况", "是这样的",
    "好的", "我已经", "了解了", "情况", "以下", "是", "我的", "分析",
    "让我", "先看看", "这个文件", "的", "内容",
    # Extended response fragments for longer content
    "经过", "仔细", "排查", "发现", "主要问题", "在于",
    "建议", "按以下", "步骤", "进行", "修改",
    "首先", "其次", "然后", "最后", "总结",
    "需要注意", "的是", "这里", "涉及", "核心逻辑",
    "实际上", "相比", "之前", "现在", "更优",
    "考虑到", "整体", "架构", "设计", "方案如下",
    "具体", "实现", "细节", "包括", "以下几点",
    "此外", "还需", "处理", "边界", "情况",
    "参考", "现有", "代码", "可以", "复用",
    "测试", "通过", "验证", "无误", "确认",
    "性能", "方面", "提升", "显著", "改善",
    "安全", "角度", "考量", "防范", "风险",
    "文档", "更新", "同步", "保持一致",
    "总体", "来看", "这次", "改动", "合理",
    "欢迎", "反馈", "任何", "问题", "随时",
    "以上", "就是", "完整", "分析", "报告",
    "代码", "片段", "示例", "展示", "用法",
    "模块", "功能", "说明", "解释", "清楚",
]


def _random_sentence(words: list[str], min_words: int, max_words: int) -> str:
    n = random.randint(min_words, max_words)
    return "".join(random.choices(words, k=n))


def _tokenize_json(text: str) -> list[str]:
    """Split a JSON string into small streaming chunks that mimic LLM output."""
    tokens = []
    i = 0
    while i < len(text):
        c = text[i]
        if c in '{}[]:,':
            # Single-char tokens: braces, brackets, colon, comma
            tokens.append(c)
            i += 1
        elif c == '"':
            # String: collect the whole quoted string (including quotes) as one token
            end = i + 1
            while end < len(text):
                if text[end] == '\\' and end + 1 < len(text):
                    end += 2
                elif text[end] == '"':
                    end += 1
                    break
                else:
                    end += 1
            # Sub-tokenize the string interior into word-like pieces
            s = text[i:end]
            # Strip surrounding quotes, tokenize interior, re-add quotes
            inner = s[1:-1]
            if inner:
                # Emit opening quote
                tokens.append('"')
                # Break inner into sub-word tokens
                j = 0
                while j < len(inner):
                    # Small groups of non-space characters
                    k = j
                    while k < len(inner) and inner[k] != ' ':
                        k += 1
                    if k > j:
                        tokens.append(inner[j:k])
                    if k < len(inner):
                        tokens.append(' ')
                        k += 1
                    j = k
                # Emit closing quote
                tokens.append('"')
            else:
                tokens.append(s)  # empty string ""
            i = end
        elif c == ' ':
            tokens.append(' ')
            i += 1
        else:
            # Other chars (numbers, etc.) - single char
            tokens.append(c)
            i += 1
    return tokens


# Tool argument templates — we pick a random command/path/value for each call
TOOL_ARG_TEMPLATES = {
    "shell": lambda: {"cmd": random.choice(SHELL_COMMANDS)},
    "read": lambda: {"path": random.choice(READ_PATHS), "offset": 1, "limit": 50},
}

TOOL_NAMES = ["shell", "read"]


# ── Helpers ──────────────────────────────────────────────────────────────────

def make_tool_call(call_id: str, name: str) -> dict:
    args = TOOL_ARG_TEMPLATES[name]()
    return {
        "id": call_id,
        "type": "function",
        "function": {
            "name": name,
            "arguments": json.dumps(args),
        },
    }


def pick_reasoning() -> str:
    return _random_sentence(REASONING_WORDS, 150, 500)


def pick_content() -> str:
    text = _random_sentence(CONTENT_WORDS, 100, 350)
    # Randomly sprinkle markdown (each type independent)
    if random.random() < 0.3:
        words = list(CONTENT_WORDS)
        code_word = random.choice(words)
        if code_word in text:
            text = text.replace(code_word, f"`{code_word}`", 1)
    if random.random() < 0.3:
        words = list(CONTENT_WORDS)
        bold_word = random.choice(words)
        if bold_word in text:
            text = text.replace(bold_word, f"**{bold_word}**", 1)
    if random.random() < 0.2:
        text += f"\n```\n{_random_sentence(CONTENT_WORDS, 3, 8)}\n```"
    if random.random() < 0.15:
        level = random.choice(["#", "##", "###"])
        text = f"{level} {_random_sentence(CONTENT_WORDS, 2, 5)}\n\n{text}"
    if random.random() < 0.2:
        items = []
        for _ in range(random.randint(2, 4)):
            items.append(f"- {_random_sentence(CONTENT_WORDS, 2, 5)}")
        text += "\n" + "\n".join(items)
    if random.random() < 0.1:
        text = f"> {_random_sentence(CONTENT_WORDS, 3, 8)}\n\n{text}"
    return text


# ── OpenAI API handler ───────────────────────────────────────────────────────

async def chat_completions_handler(request):
    try:
        body = await request.json()
    except Exception:
        return web.json_response(
            {"error": {"message": "Invalid JSON body", "type": "invalid_request_error"}},
            status=400,
        )

    # Parse common fields
    stream = body.get("stream", False)
    req_id = f"chatcmpl-{uuid.uuid4().hex[:12]}"
    model = body.get("model", "dummy-model")
    created = int(time.time())

    # Always generate tool calls (stress-test mode)
    num_tool_calls = random.randint(1, 3)
    tool_calls = []
    for i in range(num_tool_calls):
        name = random.choice(TOOL_NAMES)
        call_id = f"call_{uuid.uuid4().hex[:16]}"
        tool_calls.append(make_tool_call(call_id, name))

    reasoning = pick_reasoning()
    content = pick_content()

    if stream:
        return await _handle_streaming(
            request, web, req_id, model, created,
            reasoning, content, tool_calls,
        )
    else:
        return _handle_non_streaming(
            web, req_id, model, created,
            reasoning, content, tool_calls,
        )


async def _handle_streaming(request, web, req_id, model, created,
                            reasoning, content, tool_calls):
    """Streaming response — SSE chunks, matching real DeepSeek API format."""

    response = web.StreamResponse(
        status=200,
        reason="OK",
        headers={
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",
        },
    )
    await response.prepare(request)

    system_fp = f"fp_{uuid.uuid4().hex[:8]}"

    def make_chunk(delta: dict, finish_reason: str = None) -> str:
        chunk = {
            "id": req_id,
            "object": "chat.completion.chunk",
            "created": created,
            "model": model,
            "system_fingerprint": system_fp,
            "choices": [
                {
                    "index": 0,
                    "delta": delta,
                    "logprobs": None,
                    "finish_reason": finish_reason,
                }
            ],
        }
        return f"data: {json.dumps(chunk, ensure_ascii=False)}\n\n"

    async def send_chunk(delta: dict, finish_reason: str = None):
        await response.write(make_chunk(delta, finish_reason).encode())

    async def send_chunk_and_drain(delta: dict, finish_reason: str = None):
        await response.write(make_chunk(delta, finish_reason).encode())
        await response.drain()

    try:
        # 1. Role chunk — required before any delta
        await send_chunk({"role": "assistant", "content": ""})
        await asyncio.sleep(0.01)

        # 2. Reasoning — stream as reasoning_content if present
        if reasoning:
            for word in reasoning.split(" "):
                await send_chunk({"reasoning_content": word + " "})
                await asyncio.sleep(0.001)
            await send_chunk({"content": "\n\n"})

        # 3. Content — stream word by word
        words = content.split(" ")
        for i, word in enumerate(words):
            await send_chunk({"content": word + " "})
            if i % 5 == 0:
                await response.drain()
                await asyncio.sleep(0.002)
        await send_chunk({"content": "\n\n"})
        await response.drain()

        # 4. Tool calls
        for idx, tc in enumerate(tool_calls):
            tc_init = {
                "index": idx,
                "id": tc["id"],
                "type": "function",
                "function": {"name": tc["function"]["name"], "arguments": ""},
            }
            await send_chunk({"tool_calls": [tc_init]})
            await response.drain()
            await asyncio.sleep(0.02)

            args_str = tc["function"]["arguments"]
            tokens = _tokenize_json(args_str)
            for ti, token in enumerate(tokens):
                await send_chunk({
                    "tool_calls": [
                        {"index": idx, "function": {"arguments": token}}
                    ]
                })
                if ti % 3 == 0:
                    await response.drain()
                    await asyncio.sleep(0.001)
            await response.drain()

        # 5. Final chunk — finish_reason = "tool_calls"
        await send_chunk_and_drain({}, finish_reason="tool_calls")
        await asyncio.sleep(0.005)

        # 6. Done sentinel
        await response.write(f"data: [DONE]\n\n".encode())
        await response.drain()

    except (asyncio.CancelledError, ConnectionResetError):
        pass

    return response


def _handle_non_streaming(web, req_id, model, created,
                          reasoning, content, tool_calls):
    """Non-streaming response — single JSON body."""

    msg = {
        "role": "assistant",
        "content": content,
    }

    if reasoning:
        msg["reasoning"] = reasoning

    if tool_calls:
        msg["tool_calls"] = tool_calls

    resp_body = {
        "id": req_id,
        "object": "chat.completion",
        "created": created,
        "model": model,
        "system_fingerprint": f"fp_{uuid.uuid4().hex[:8]}",
        "choices": [
            {
                "index": 0,
                "message": msg,
                "finish_reason": "tool_calls" if tool_calls else "stop",
                "logprobs": None,
            }
        ],
        "usage": {
            "prompt_tokens": random.randint(50, 500),
            "completion_tokens": random.randint(100, 500),
            "total_tokens": random.randint(150, 1000),
        },
    }

    return web.json_response(resp_body)


# ── Health / info endpoints ──────────────────────────────────────────────────

async def models_handler(request):
    resp = {
        "object": "list",
        "data": [
            {
                "id": "dummy-model",
                "object": "model",
                "created": int(time.time()),
                "owned_by": "dummy-org",
                "permission": [],
                "root": "dummy-model",
                "parent": None,
            }
        ],
    }
    return web.json_response(resp)


async def root_handler(request):
    html = """<!DOCTYPE html>
<html>
<head>
    <title>Dummy OpenAI API Server</title>
    <style>
        body { font-family: monospace; margin: 40px; line-height: 1.6; }
        code { background: #f0f0f0; padding: 2px 6px; border-radius: 3px; }
        .endpoint { margin: 10px 0; padding: 10px; background: #f8f9fa;
                     border-left: 3px solid #0d6efd; }
        h2 { color: #333; }
    </style>
</head>
<body>
    <h1>Dummy OpenAI API Server</h1>
    <p>Returns randomly generated reasoning, content, and executable shell tool calls.</p>

    <h2>Endpoints</h2>
    <div class="endpoint">
        <strong>POST /v1/chat/completions</strong><br>
        OpenAI-compatible chat completions endpoint.<br>
        Supports both <code>stream: true</code> (SSE) and <code>stream: false</code>.<br>
        Example:
        <pre>curl -X POST http://127.0.0.1:{port}/v1/chat/completions \\
  -H "Content-Type: application/json" \\
  -d '{{"model":"dummy-model","messages":[{{"role":"user","content":"hello"}}],"stream":true}}'</pre>
    </div>
    <div class="endpoint">
        <strong>GET /v1/models</strong><br>
        Returns available models (just dummy-model).
    </div>
</body>
</html>"""
    return web.Response(text=html.format(port=request.url.port or 5344),
                        content_type="text/html")


# ── Main ─────────────────────────────────────────────────────────────────────

async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5344

    app = web.Application(client_max_size=256*1024*1024)  # 256MB for stress-test
    app.router.add_post("/v1/chat/completions", chat_completions_handler)
    app.router.add_get("/v1/models", models_handler)
    app.router.add_get("/", root_handler)

    print(f"Dummy OpenAI API Server running on http://127.0.0.1:{port}")
    print(f"Endpoints:")
    print(f"  POST /v1/chat/completions  (OpenAI chat completions API)")
    print(f"  GET  /v1/models             (List models)")
    print(f"  GET  /                      (Info page)")
    print(f"\nPress Ctrl+C to stop")

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", port)
    await site.start()

    while True:
        await asyncio.sleep(3600)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped")
