#!/usr/bin/env python3
"""Serve EuroBERT embeddings and an encoder-similarity ``POST /rerank`` control.

The same attention-masked mean pooling and L2 normalization is used before and
after reranker training. This measures whether one EuroBERT checkpoint can serve
both retrieval and cross-encoder reranking roles. Encoder-similarity reranking is
a quality control, not a latency-equivalent cross-encoder baseline.
"""

from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from serve_cross_encoder import RerankBatcher, validate_pairs


def ordered_unique_texts(pairs: list[list[str]]) -> list[str]:
    return list(dict.fromkeys(text for pair in pairs for text in pair))


class BiEncoderModel:
    def __init__(self, model: Any, tokenizer: Any, device: str, max_length: int):
        self.model = model
        self.tokenizer = tokenizer
        self.device = device
        self.max_length = max_length
        self.lock = threading.Lock()

    def embed(self, texts: list[str], batch_size: int):
        import torch
        import torch.nn.functional as functional

        vectors = []
        with self.lock, torch.inference_mode():
            for offset in range(0, len(texts), batch_size):
                batch = texts[offset : offset + batch_size]
                encoded = self.tokenizer(
                    batch,
                    padding=True,
                    truncation=True,
                    max_length=self.max_length,
                    return_tensors="pt",
                ).to(self.device)
                hidden = self.model(**encoded).last_hidden_state.float()
                mask = encoded["attention_mask"].unsqueeze(-1).to(hidden.dtype)
                pooled = (hidden * mask).sum(dim=1) / mask.sum(dim=1).clamp_min(1.0)
                vectors.append(functional.normalize(pooled, p=2, dim=1).cpu())
        return torch.cat(vectors, dim=0).numpy()

    def predict(
        self,
        pairs: list[list[str]],
        *,
        batch_size: int,
        show_progress_bar: bool,
        convert_to_numpy: bool,
    ):
        del show_progress_bar
        import numpy as np
        texts = ordered_unique_texts(pairs)
        matrix = self.embed(texts, batch_size)
        by_text = {text: matrix[index] for index, text in enumerate(texts)}
        scores = np.asarray(
            [float(np.dot(by_text[query], by_text[document])) for query, document in pairs],
            dtype=np.float32,
        )
        return scores if convert_to_numpy else scores.tolist()


def verify_model_identity(model: Any, model_type: str, hidden_size: int, layers: int) -> None:
    expected = {
        "model_type": model_type,
        "hidden_size": hidden_size,
        "num_hidden_layers": layers,
    }
    actual = {key: getattr(model.config, key, None) for key in expected}
    if actual != expected:
        raise RuntimeError(f"loaded model identity mismatch: expected {expected}, received {actual}")


def embedding_texts(value: Any) -> list[str]:
    if not isinstance(value, dict):
        raise ValueError("embedding request must be a JSON object")
    inputs = value.get("input")
    if isinstance(inputs, str):
        return [inputs]
    if not isinstance(inputs, list) or not all(isinstance(text, str) for text in inputs):
        raise ValueError("embedding input must be a string or an array of strings")
    if not inputs:
        raise ValueError("embedding input must not be empty")
    return inputs


def handler_for_biencoder(
    batcher: RerankBatcher,
    scorer: BiEncoderModel,
    health: dict[str, Any],
    batch_size: int,
):
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
            try:
                size = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(size))
                path = self.path.rstrip("/")
                if path == "/rerank":
                    self.send_json(200, batcher.submit(validate_pairs(payload)))
                    return
                if path == "/v1/embeddings":
                    texts = embedding_texts(payload)
                    vectors = scorer.embed(texts, batch_size)
                    self.send_json(
                        200,
                        {
                            "object": "list",
                            "model": payload.get("model", health["model"]),
                            "data": [
                                {"object": "embedding", "index": index, "embedding": vector.tolist()}
                                for index, vector in enumerate(vectors)
                            ],
                            "usage": {"prompt_tokens": 0, "total_tokens": 0},
                        },
                    )
                    return
                self.send_json(404, {"error": "not found"})
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
    parser.add_argument(
        "--model-state",
        choices=("official_pretrained_base", "ettin_teacher_score_finetuned"),
        default="official_pretrained_base",
    )
    parser.add_argument("--expected-model-type", default="eurobert")
    parser.add_argument("--expected-hidden-size", type=int, required=True)
    parser.add_argument("--expected-layers", type=int, required=True)
    parser.add_argument("--port", type=int, default=8920)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--maximum-wait-ms", type=int, default=10)
    parser.add_argument("--max-length", type=int, default=512)
    args = parser.parse_args()

    import torch
    from transformers import AutoModel, AutoModelForSequenceClassification, AutoTokenizer

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA device is not available through torch.cuda")
    revision_kwargs = {"revision": args.revision} if args.revision else {}
    tokenizer = AutoTokenizer.from_pretrained(args.model, **revision_kwargs)
    model_kwargs = {
        "dtype": torch.bfloat16,
        "attn_implementation": "sdpa",
        **revision_kwargs,
    }
    if args.model_state == "ettin_teacher_score_finetuned":
        classifier = AutoModelForSequenceClassification.from_pretrained(args.model, **model_kwargs)
        model = classifier.base_model
    else:
        model = AutoModel.from_pretrained(args.model, **model_kwargs)
    model = model.to("cuda")
    model.eval()
    verify_model_identity(
        model,
        args.expected_model_type,
        args.expected_hidden_size,
        args.expected_layers,
    )
    scorer = BiEncoderModel(model, tokenizer, "cuda", args.max_length)
    batcher = RerankBatcher(scorer, args.batch_size, args.maximum_wait_ms)
    health = {
        "status": "ready",
        "role": "embed_and_encoder_similarity_rerank",
        "model": args.model,
        "revision": args.revision,
        "model_state": args.model_state,
        "scoring": "cosine_similarity",
        "pooling": "attention_masked_mean_last_hidden_state_then_l2_normalize",
        "latency_comparability": "not_cross_encoder_comparable",
        "device": torch.cuda.get_device_name(0),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "hip": torch.version.hip,
        "dtype": "bfloat16",
        "max_length": args.max_length,
        "batch_size": args.batch_size,
        "maximum_wait_ms": args.maximum_wait_ms,
    }
    server = ThreadingHTTPServer(
        ("0.0.0.0", args.port),
        handler_for_biencoder(batcher, scorer, health, args.batch_size),
    )
    print(json.dumps(health, sort_keys=True), flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
