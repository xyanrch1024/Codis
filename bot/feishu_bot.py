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
import asyncio
import logging
from urllib.parse import urljoin

import httpx
import lark_oapi as lark

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("feishu-bot")

CODIS_SERVER = os.environ.get("CODIS_SERVER", "http://127.0.0.1:8711")
CHAT_SESSIONS = {}  # chat_id → session_id
LAST_MSG_IDS = {}   # chat_id → msg_id 去重


async def codis_create_session() -> str | None:
    async with httpx.AsyncClient(timeout=10) as cli:
        r = await cli.post(urljoin(CODIS_SERVER, "/api/v1/sessions"))
        if r.status_code == 201:
            return r.json()["session_id"]
    return None


async def codis_send(session_id: str, text: str):
    async with httpx.AsyncClient(timeout=10) as cli:
        body = {
            "session_id": session_id,
            "messages": [{"role": "user", "content": text}]
        }
        await cli.post(urljoin(CODIS_SERVER, "/api/v1/acp"), json=body)


async def handle_message(event: lark.im.v1.P2ImMessageReceiveV1):
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

    # session 映射
    if chat_id not in CHAT_SESSIONS:
        sid = await codis_create_session()
        if sid:
            CHAT_SESSIONS[chat_id] = sid
            log.info(f"session {sid[:8]} <-> chat {chat_id[:8]}")

    sid = CHAT_SESSIONS.get(chat_id)
    if sid:
        await codis_send(sid, text)


def main():
    app_id = os.environ["FEISHU_APP_ID"]
    app_secret = os.environ["FEISHU_APP_SECRET"]

    def on_any(event: lark.CustomizedEvent) -> None:
        log.info(f"!!! CUSTOM EVENT: type={event.header.event_type} body={event.body[:300]}")

    handler = (
        lark.EventDispatcherHandler.builder("", "")
        .register_p2_im_message_receive_v1(handle_message)
        .register_p1_customized_event("im.chat.access_event.bot_p2p_chat_entered_v1", on_any)
        .build()
    )

    cli = lark.ws.Client(
        app_id, app_secret,
        event_handler=handler,
        log_level=lark.LogLevel.INFO,
    )

    log.info("Feishu bot starting...")
    log.info(f"Codis server: {CODIS_SERVER}")
    cli.start()


if __name__ == "__main__":
    main()
