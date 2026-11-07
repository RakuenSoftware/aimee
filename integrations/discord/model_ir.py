"""Canonical model values at Discord ingress and the delivery text view.

These are the existing Aimee Messages, not a provider protocol.
The thinclient validates and transports them; no provider codec lives here.
"""
from copy import deepcopy
from dataclasses import dataclass


@dataclass
class ModelTurn:
    message: dict
    messages: list[dict]
    delivery_notice: str = ""


def chat_turn(messages):
    # The shared thinclient admits the completed stream. Keep its canonical
    # Messages intact; the final assistant Message is only the delivery view.
    if (not isinstance(messages, list) or not messages
            or any(not isinstance(item, dict) for item in messages)):
        raise RuntimeError("Aimee returned no complete canonical turn")
    final = messages[-1]
    if (final.get("role") != "assistant" or not isinstance(final.get("blocks"), list)
            or any(not isinstance(block, dict) or block.get("type") not in ("text", "thinking")
                   for block in final["blocks"])):
        raise RuntimeError("Aimee returned no final canonical assistant message")
    owned = deepcopy(messages)
    return ModelTurn(owned[-1], owned)


def text_block(text, *, origin=3, authority=2):
    return {"type": "text", "text": text, "context": {
        "origin": origin, "authority": authority, "trust": 2,
        "sensitivity": 1, "model_visible": True,
        "revision_domain": "", "revision_scope": "", "revision_epoch": "0"}}


def message(role, text, *, origin=None):
    return {"role": role, "blocks": [text_block(
        text, origin=origin if origin is not None else (7 if role == "assistant" else 3),
        authority=1 if role == "assistant" else 2)]}


def display_text(value):
    """Read-only delivery/query view; never reconstruct a model Message from it."""
    notice = value.delivery_notice if isinstance(value, ModelTurn) else ""
    value = value.message if isinstance(value, ModelTurn) else value
    return notice + "".join(block.get("text", "") for block in value["blocks"]
                   if block.get("type") == "text"
                   and block.get("context", {}).get("model_visible", True)
                   and block.get("context", {}).get("sensitivity", 0) < 3)


def chat_message(policy, task, evidence=()):
    # Client policy is an unverified operator instruction, not host policy.
    blocks = [text_block(policy, origin=2), text_block(task)]
    blocks.extend(text_block(text, origin=origin, authority=1)
                  for origin, text in evidence if text)
    return {"role": "user", "blocks": blocks}
