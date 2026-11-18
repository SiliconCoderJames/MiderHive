#!/usr/bin/env python3
"""本地 LLM mock 服务器：手动验证「设置 → LLM 接入」与临时问答对话框用。

用法：
    python scripts/llm_mock_server.py            # 监听 127.0.0.1:8788
然后在 设置 → LLM 接入 里：
    服务商 = 自定义 OpenAI 兼容端点
    接口地址 = http://127.0.0.1:8788
    模型 = mock-model，密钥任意（如 mock-key）
「测试连接」与临时问答对话框都会拿到流式回复；换协议下拉为 Anthropic 即验 /v1/messages。

仅监听 127.0.0.1（与平台同一纪律）；不落任何盘；无任何真实凭据。
"""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8788


class Handler(BaseHTTPRequestHandler):
    def _sse(self, chunks, done=False):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for c in chunks:
            self.wfile.write(b"data: " + json.dumps(c).encode("utf-8") + b"\n\n")
            self.wfile.flush()
            time.sleep(0.05)  # 让流式效果肉眼可见
        if done:
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

    def _json(self, obj, code=200):
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        stream = bool(body.get("stream"))
        if self.path == "/chat/completions":  # OpenAI 兼容形状
            if stream:
                self._sse(
                    [
                        {"choices": [{"delta": {"content": "你好"}}]},
                        {"choices": [{"delta": {"content": "！mock 回复"}}]},
                        {"choices": [],
                         "usage": {"prompt_tokens": 12, "completion_tokens": 3}},
                    ],
                    done=True,
                )
            else:
                self._json({"choices": [{"message": {"role": "assistant",
                                                     "content": "你好！mock 回复"}}],
                            "usage": {"prompt_tokens": 12, "completion_tokens": 3}})
        elif self.path == "/v1/messages":  # Anthropic 形状
            if stream:
                self._sse(
                    [
                        {"type": "message_start",
                         "message": {"usage": {"input_tokens": 10}}},
                        {"type": "content_block_delta",
                         "delta": {"type": "text_delta", "text": "Bonjour (mock)"}},
                        {"type": "message_delta", "usage": {"output_tokens": 2}},
                        {"type": "message_stop"},
                    ]
                )
            else:
                self._json({"content": [{"type": "text", "text": "Bonjour (mock)"}],
                            "usage": {"input_tokens": 10, "output_tokens": 2}})
        else:
            self._json({"error": {"message": "unknown path"}}, 404)

    def log_message(self, *a):  # 安静模式
        pass


if __name__ == "__main__":
    print("LLM mock server on http://127.0.0.1:%d "
          "(OpenAI /chat/completions + Anthropic /v1/messages)" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
