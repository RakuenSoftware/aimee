#!/usr/bin/env python3
"""Serve a CrossEncoder behind AIMEE's aligned ``POST /rerank`` contract."""

from __future__ import annotations

import argparse
import json
import queue
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


@dataclass
class Job:
    pairs: list[list[str]]
    ready: threading.Event = field(default_factory=threading.Event)
    scores: list[float] | None = None
    error: str = ""


class RerankBatcher:
    def __init__(self, model: Any, batch_size: int, maximum_wait_ms: int):
        self.model = model
        self.batch_size = batch_size
        self.maximum_wait_s = maximum_wait_ms / 1000.0
        self.jobs: queue.Queue[Job] = queue.Queue()
        self.thread = threading.Thread(target=self._run, name="rerank-batcher", daemon=True)
        self.thread.start()

    def submit(self, pairs: list[list[str]]) -> list[float]:
        if not pairs:
            return []
        job = Job(pairs=pairs)
        self.jobs.put(job)
        job.ready.wait()
        if job.error:
            raise RuntimeError(job.error)
        if job.scores is None:
            raise RuntimeError("reranking job completed without scores")
        return job.scores

    def _run(self) -> None:
        while True:
            first = self.jobs.get()
            batch = [first]
            pair_count = len(first.pairs)
            deadline = time.monotonic() + self.maximum_wait_s
            while pair_count < self.batch_size:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    candidate = self.jobs.get(timeout=remaining)
                except queue.Empty:
                    break
                if pair_count + len(candidate.pairs) > self.batch_size:
                    self.jobs.put(candidate)
                    break
                batch.append(candidate)
                pair_count += len(candidate.pairs)
            pairs = [pair for job in batch for pair in job.pairs]
            try:
                values = self.model.predict(
                    pairs,
                    batch_size=self.batch_size,
                    show_progress_bar=False,
                    convert_to_numpy=True,
                )
                scores = [float(value) for value in values.reshape(-1)]
                offset = 0
                for job in batch:
                    job.scores = scores[offset : offset + len(job.pairs)]
                    offset += len(job.pairs)
            except Exception as exc:  # noqa: BLE001 - returned to each waiting request
                message = f"{type(exc).__name__}: {exc}"
                for job in batch:
                    job.error = message
            finally:
                for job in batch:
                    job.ready.set()


def validate_pairs(value: Any) -> list[list[str]]:
    if not isinstance(value, list):
        raise ValueError("rerank expects a JSON array")
    pairs: list[list[str]] = []
    for item in value:
        if not isinstance(item, list) or len(item) != 2 or not all(isinstance(text, str) for text in item):
            raise ValueError("each rerank item must be [query, candidate] strings")
        pairs.append(item)
    return pairs


def handler_for(batcher: RerankBatcher, health: dict[str, Any]):
    class Handler(BaseHTTPRequestHandler):
        def send_json(self, status: int, payload: Any) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802 - stdlib handler contract
            if self.path.rstrip("/") in {"", "/health", "/health/rerank"}:
                self.send_json(200, health)
            else:
                self.send_json(404, {"error": "not found"})

        def do_POST(self) -> None:  # noqa: N802 - stdlib handler contract
            if self.path.rstrip("/") != "/rerank":
                self.send_json(404, {"error": "not found"})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                pairs = validate_pairs(json.loads(self.rfile.read(size)))
                self.send_json(200, batcher.submit(pairs))
            except (ValueError, json.JSONDecodeError) as exc:
                self.send_json(400, {"error": str(exc)})
            except RuntimeError as exc:
                self.send_json(500, {"error": str(exc)})

        def log_message(self, *_args: Any) -> None:
            return

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--revision")
    parser.add_argument("--port", type=int, default=8920)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--maximum-wait-ms", type=int, default=10)
    parser.add_argument("--max-length", type=int, default=512)
    args = parser.parse_args()

    import torch
    from sentence_transformers import CrossEncoder

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA device is not available through torch.cuda")
    model = CrossEncoder(
        args.model,
        revision=args.revision,
        device="cuda",
        max_length=args.max_length,
        activation_fn=torch.nn.Identity(),
        model_kwargs={"torch_dtype": torch.bfloat16, "attn_implementation": "sdpa"},
    )
    batcher = RerankBatcher(model, args.batch_size, args.maximum_wait_ms)
    health = {
        "status": "ready",
        "role": "rerank",
        "model": args.model,
        "revision": args.revision,
        "device": torch.cuda.get_device_name(0),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "hip": torch.version.hip,
        "dtype": "bfloat16",
        "max_length": args.max_length,
        "batch_size": args.batch_size,
        "maximum_wait_ms": args.maximum_wait_ms,
    }
    server = ThreadingHTTPServer(("0.0.0.0", args.port), handler_for(batcher, health))
    print(json.dumps(health, sort_keys=True), flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
