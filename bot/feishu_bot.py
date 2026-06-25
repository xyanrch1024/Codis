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
    with httpx.Client(timeout=10, trust_env=False) as cli:
        r = cli.post(urljoin(CODIS_SERVER, "/api/v1/sessions"))
        if r.status_code == 201:
            return r.json()["session_id"]
    return None


def codis_chat(text: str) -> str | None:
    """同步调用 Codis /chat 端点，等待完整回复"""
    try:
        with httpx.Client(timeout=120, trust_env=False) as cli:
            body = {
                "provider": "glm",
                "model": "glm-4.5-flash",
                "messages": [{"role": "user", "content": text}],
                "max_tokens": 1000,
            }
            r = cli.post(urljoin(CODIS_SERVER, "/api/v1/chat"), json=body)
            if r.status_code == 200:
                data = r.json()
                log.info(f"Chat response: {json.dumps(data)[:200]}")
                return data.get("content", "")
            else:
                log.error(f"Chat failed: {r.status_code} {r.text[:200]}")
    except Exception as e:
        log.error(f"codis_chat error: {e}")
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

    # 调用 LLM
    reply = codis_chat(text)
    if reply:
        log.info(f"LLM reply: {reply[:50]}")
        feishu_send_message(chat_id, reply)
    else:
        log.error("LLM returned empty")


def main():
    app_id = os.environ["FEISHU_APP_ID"]
    app_secret = os.environ["FEISHU_APP_SECRET"]

    def on_any(event: lark.CustomizedEvent) -> None:
        log.info(f"!!! CUSTOM EVENT: type={event.header.event_type} body={event.body[:300]}")

    handler = (
        lark.EventDispatcherHandler.builder("", "")
        .register_p2_im_message_receive_v1(handle_message)
        .register_p1_customized_event("im.chat.access_event.bot_p2p_chat_entered_v1",
            lambda e: log.info(f"bot entered chat"))
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
