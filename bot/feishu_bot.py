#!/usr/bin/env python3
"""
Feishu Bot — 飞书 ↔ Python Bot ↔ Codis C++ Server

环境变量:
  FEISHU_APP_ID     飞书 App ID
  FEISHU_APP_SECRET  飞书 App Secret
  CODIS_SERVER       Codis Server URL (默认 http://127.0.0.1:8711)

Usage:
  python3 feishu_bot.py
"""

import os
import json
import logging
from urllib.parse import urljoin

import httpx
import lark_oapi as lark

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("feishu-bot")

# 日志级别
_log_level = os.environ.get("LOG_LEVEL", "info").upper()
_log_map = {"TRACE": logging.DEBUG, "DEBUG": logging.DEBUG, "INFO": logging.INFO,
            "WARN": logging.WARNING, "ERROR": logging.ERROR}
_log_level = _log_map.get(_log_level, logging.INFO)
_log_level = logging.DEBUG if _log_level == "TRACE" else _log_level  # debug for trace
logging.getLogger().setLevel(_log_level)

# 飞书 SDK 日志级别
_lark_level = lark.LogLevel.DEBUG if _log_level <= logging.DEBUG else lark.LogLevel.INFO

CODIS_SERVER = os.environ.get("CODIS_SERVER", "http://127.0.0.1:8711")
CHAT_SESSIONS = {}  # chat_id → session_id
LAST_MSG_IDS = {}   # chat_id → msg_id 去重
TOKEN = {"value": "", "expire": 0}
TOKEN_LOCK = __import__('threading').Lock()


def get_tenant_token() -> str:
    """获取飞书 tenant_access_token (带缓存)"""
    import time
    with TOKEN_LOCK:
        if TOKEN["value"] and time.time() < TOKEN["expire"]:
            return TOKEN["value"]

        with httpx.Client(timeout=10, trust_env=False) as cli:
            r = cli.post("https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
                json={"app_id": os.environ["FEISHU_APP_ID"],
                       "app_secret": os.environ["FEISHU_APP_SECRET"]})
            if r.status_code == 200:
                data = r.json()
                if data.get("code") == 0:
                    TOKEN["value"] = data["tenant_access_token"]
                    TOKEN["expire"] = time.time() + data.get("expire", 7200) - 60
                    return TOKEN["value"]
    return ""


def feishu_send_message(chat_id: str, text: str):
    """发送消息到飞书群聊"""
    token = get_tenant_token()
    if not token:
        log.error("No tenant token")
        return

    body = {
        "receive_id": chat_id,
        "msg_type": "text",
        "content": json.dumps({"text": text})
    }

    with httpx.Client(timeout=10, trust_env=False) as cli:
        r = cli.post(
            "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
            headers={"Authorization": f"Bearer {token}"},
            json=body)
        if r.status_code == 200:
            log.info(f"Reply sent to {chat_id[:8]}")
        else:
            log.error(f"Send failed: {r.status_code} {r.text[:100]}")


def codis_acp_chat(chat_id: str, text: str) -> str | None:
    """ACP 多轮对话：POST /api/v1/acp → GET /api/v1/acp/stream/{sid}"""

    # 每个 chat_id 绑定一个 session，实现多轮上下文
    sid = CHAT_SESSIONS.get(chat_id)
    if not sid:
        try:
            with httpx.Client(timeout=10, trust_env=False) as cli:
                r = cli.post(urljoin(CODIS_SERVER, "/api/v1/sessions"))
                if r.status_code == 201:
                    sid = r.json()["session_id"]
                    CHAT_SESSIONS[chat_id] = sid
        except Exception:
            pass
    if not sid:
        log.error("Failed to create session")
        return None

    try:
        with httpx.Client(timeout=10, trust_env=False) as cli:
            # Step 1: POST /api/v1/acp (fire-and-forget)
            body = {
                "provider": "glm",
                "model": "glm-4.5-flash",
                "session_id": sid,
                "messages": [{"role": "user", "content": text}],
                "max_tokens": 1000,
            }
            r = cli.post(urljoin(CODIS_SERVER, "/api/v1/acp"), json=body)
            if r.status_code not in (200, 202):
                log.error(f"ACP POST failed: {r.status_code}")
                return None

        # Step 2: GET SSE stream → 阻塞读直到 done
        with httpx.Client(timeout=120, trust_env=False) as sse_cli:
            content_parts = []
            url = urljoin(CODIS_SERVER, f"/api/v1/acp/stream/{sid}")

            with sse_cli.stream("GET", url) as sse:
                for line in sse.iter_lines():
                    if not line or not line.startswith("data: "):
                        continue
                    try:
                        frame = json.loads(line[6:])  # skip "data: " prefix
                        event_type = frame.get("type")
                        data = frame.get("data", {})

                        if event_type == "assistant":
                            content_parts.append(data.get("delta", ""))
                        elif event_type == "tool_call":
                            log.info(f"Tool call: {data.get('name')}")
                        elif event_type == "tool_result":
                            ok = data.get("success", False)
                            log.info(f"Tool result: {'ok' if ok else 'fail'}")
                        elif event_type == "error":
                            log.error(f"ACP error: {data.get('message', '')}")
                            return None
                        elif event_type == "done":
                            break
                    except (json.JSONDecodeError, KeyError):
                        continue

                result = "".join(content_parts)
                if result:
                    log.info(f"ACP response: {result[:100]}")
                    return result
    except Exception as e:
        log.error(f"codis_acp_chat error: {e}")
    return None


def handle_message(event: lark.im.v1.P2ImMessageReceiveV1):
    log.info(f"!!! GOT MESSAGE v2 !!!")
    msg = event.event.message
    chat_id = msg.chat_id
    msg_id = msg.message_id
    msg_id = msg.message_id
    content = json.loads(msg.content)
    text = content.get("text", "")
    log.info(f"recieve msg: {text}")

    # 去重
    if LAST_MSG_IDS.get(chat_id) == msg_id:
        return
    LAST_MSG_IDS[chat_id] = msg_id

    if not text:
        return

    log.info(f"[{chat_id[:8]}] {text[:50]}")

    # 调用 LLM（ACP 多轮，支持 tool）
    reply = codis_acp_chat(chat_id, text)
    if reply:
        log.info(f"LLM reply: {reply[:50]}")
        feishu_send_message(chat_id, reply)
    else:
        log.error("LLM returned empty")


def main():
    app_id = os.environ["FEISHU_APP_ID"]
    app_secret = os.environ["FEISHU_APP_SECRET"]

    handler = (
        lark.EventDispatcherHandler.builder("", "")
        .register_p2_im_message_receive_v1(handle_message)
        .register_p2_im_chat_access_event_bot_p2p_chat_entered_v1(
            lambda e: log.debug("bot entered chat"))
        .register_p2_im_message_message_read_v1(
            lambda e: log.debug("message read"))
        .build()
    )

    cli = lark.ws.Client(
        app_id, app_secret,
        event_handler=handler,
        log_level=_lark_level,
    )

    log.info("Feishu bot starting...")
    log.info(f"Codis server: {CODIS_SERVER}")
    cli.start()


if __name__ == "__main__":
    main()
