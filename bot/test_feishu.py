#!/usr/bin/env python3
"""Debug: catch both v1 and v2 Feishu events"""
import os
import lark_oapi as lark

def on_p2_im(event: lark.im.v1.P2ImMessageReceiveV1) -> None:
    print(f">>> P2 (v2) MESSAGE received: {event}")

def on_p1_im(event: lark.CustomizedEvent) -> None:
    print(f">>> P1 (v1) CUSTOM: type={event.header.event_type} body={event.body[:200]}")

def main():
    handler = (
        lark.EventDispatcherHandler.builder("", "")
        .register_p2_im_message_receive_v1(on_p2_im)
        .register_p1_customized_event("im.message.receive_v1", on_p1_im)
        .build()
    )
    cli = lark.ws.Client(
        os.environ["FEISHU_APP_ID"],
        os.environ["FEISHU_APP_SECRET"],
        event_handler=handler,
        log_level=lark.LogLevel.DEBUG,
    )
    print("Bot started. Send message in Feishu!")
    cli.start()

if __name__ == "__main__":
    main()
