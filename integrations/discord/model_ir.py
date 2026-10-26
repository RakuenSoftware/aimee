"""Canonical model values at Discord ingress and the delivery text view.

These are the existing Aimee Request/Message shapes, not a provider protocol.
The thinclient validates and transports them; no provider codec lives here.
"""
from copy import deepcopy
from dataclasses import dataclass


@dataclass
class ModelTurn:
    message: dict
    response: dict


def response_turn(response):
    # ResponseBlock.raw is response-only inert custody. Message's typed fields
    # are the same canonical IR; keep the complete Response alongside its view.
    if (response.get("schema") != "aimee.response.v1" or response.get("role") not in (None, "assistant")
            or response.get("stop_reason") not in (1, 4)
            or response.get("error_message") or response.get("refusal")
            or not isinstance(response.get("blocks"), list)
            or any(not isinstance(block, dict) or block.get("type") not in ("text", "thinking")
                   for block in response["blocks"])):
        raise RuntimeError("Aimee returned no complete canonical response")
    blocks = [{key: deepcopy(value) for key, value in block.items() if key != "raw"}
              for block in response["blocks"]]
    return ModelTurn({"role": "assistant", "blocks": blocks}, deepcopy(response))


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
    value = value.message if isinstance(value, ModelTurn) else value
    return "".join(block.get("text", "") for block in value["blocks"]
                   if block.get("type") == "text"
                   and block.get("context", {}).get("model_visible", True)
                   and block.get("context", {}).get("sensitivity", 0) < 3)


def request(policy, history, task, evidence=()):
    blocks = [text_block(task)]
    blocks.extend(text_block(text, origin=origin, authority=1)
                  for origin, text in evidence if text)
    return {"version": 1, "source_wire": "unspecified", "cache_policy": "native",
            "system": [text_block(policy, origin=2, authority=3)],
            "messages": deepcopy(history) + [{"role": "user", "blocks": blocks}],
            "tools": [], "tool_choice": {"mode": "none"}, "max_tokens": 384,
            "temperature": 0.5, "stream": False, "stop_sequences": []}
