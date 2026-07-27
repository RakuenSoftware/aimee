#!/usr/bin/env python3
"""Serve a stock EuroBERT encoder-similarity control via ``POST /rerank``.

The official base checkpoints do not contain a trained scalar reranking head.
This control therefore mean-pools the deterministic base-model hidden states,
L2 normalizes them, and scores query/document pairs with cosine similarity.
It is a quality control, not a latency-equivalent cross-encoder baseline.
"""

from __future__ import annotations

import argparse
import json
from http.server import ThreadingHTTPServer
from typing import Any

from serve_cross_encoder import RerankBatcher, handler_for


def ordered_unique_texts(pairs: list[list[str]]) -> list[str]:
    return list(dict.fromkeys(text for pair in pairs for text in pair))


class BiEncoderModel:
    def __init__(self, model: Any, tokenizer: Any, device: str, max_length: int):
        self.model = model
        self.tokenizer = tokenizer
        self.device = device
        self.max_length = max_length

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
        import torch
        import torch.nn.functional as functional

        texts = ordered_unique_texts(pairs)
        vectors = []
        with torch.inference_mode():
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
        matrix = torch.cat(vectors, dim=0)
        by_text = {text: matrix[index] for index, text in enumerate(texts)}
        scores = np.asarray(
            [float(torch.dot(by_text[query], by_text[document])) for query, document in pairs],
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--expected-model-type", default="eurobert")
    parser.add_argument("--expected-hidden-size", type=int, required=True)
    parser.add_argument("--expected-layers", type=int, required=True)
    parser.add_argument("--port", type=int, default=8920)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--maximum-wait-ms", type=int, default=10)
    parser.add_argument("--max-length", type=int, default=512)
    args = parser.parse_args()

    import torch
    from transformers import AutoModel, AutoTokenizer

    if not torch.cuda.is_available():
        raise RuntimeError("ROCm device is not available through torch.cuda")
    tokenizer = AutoTokenizer.from_pretrained(args.model, revision=args.revision)
    model = AutoModel.from_pretrained(
        args.model,
        revision=args.revision,
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
    ).to("cuda")
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
        "role": "rerank",
        "model": args.model,
        "revision": args.revision,
        "model_state": "official_pretrained_base",
        "scoring": "cosine_similarity",
        "pooling": "attention_masked_mean_last_hidden_state_then_l2_normalize",
        "latency_comparability": "not_cross_encoder_comparable",
        "device": torch.cuda.get_device_name(0),
        "torch": torch.__version__,
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
