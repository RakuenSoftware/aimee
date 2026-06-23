#!/usr/bin/env python3
"""Ettin reranker scoring head — the sentence-transformers Dense head applied on
top of the llama.cpp encoder output.

unified-llm-container P2 finding (benchmarks/results/unified-llm/P2-serving-validation.md):
`cross-encoder/ettin-reranker-{400m,68m}-v1` are sentence-transformers models whose
relevance-score head is a module pipeline that does NOT survive `convert_hf_to_gguf.py`
(a naive GGUF is encoder-only and misranks). So the unified container serves the ettin
ENCODER on llama.cpp (`/v1/embeddings`, CLS pooling, on `query</s>doc`) and applies this
small head in the gateway — pure numpy, no torch.

Head (from the model's `modules.json`): pooled-CLS vector `v` (dim D) ->
  2_Dense:    h = GELU(v @ W2^T)            (D -> D, no bias)
  3_LayerNorm:h = (h - mean)/std * gamma + beta
  4_Dense:    s = h @ W4^T + b4             (D -> 1)  => scalar relevance score

Weights load from the model's sentence-transformers dirs (`2_Dense/`, `3_LayerNorm/`,
`4_Dense/` safetensors); ~4 MB, baked into the image alongside the encoder GGUF.
"""
import os

import numpy as np


def gelu(x):
    """Tanh-approx GELU (matches what the validated P2 head used)."""
    return 0.5 * x * (1.0 + np.tanh(np.sqrt(2.0 / np.pi) * (x + 0.044715 * x * x * x)))


def layer_norm(x, gamma, beta, eps=1e-5):
    """LayerNorm over the last axis (sentence-transformers LayerNorm module)."""
    mean = x.mean(axis=-1, keepdims=True)
    std = x.std(axis=-1, keepdims=True)
    return (x - mean) / (std + eps) * gamma + beta


class EttinRerankHead:
    """The 2_Dense -> 3_LayerNorm -> 4_Dense scoring head.

    `W2` (D,D), `W4` (1,D) + `b4` (1,), `gamma`/`beta` (D,). `score()` takes one
    encoder vector (or a batch, shape (N,D)) and returns the scalar score(s).
    Construct via `from_dir()` (loads the ST safetensors) or directly with arrays
    (for tests). The encoder vector is the CLS-pooled `/v1/embeddings` output for
    `query</s>doc`.
    """

    def __init__(self, w2, w4, b4, gamma=None, beta=None):
        self.W2 = np.asarray(w2, dtype=np.float32)
        self.W4 = np.asarray(w4, dtype=np.float32)
        self.b4 = np.float32(0.0 if b4 is None else np.asarray(b4, dtype=np.float32).reshape(-1)[0])
        self.gamma = None if gamma is None else np.asarray(gamma, dtype=np.float32)
        self.beta = None if beta is None else np.asarray(beta, dtype=np.float32)
        if self.W2.ndim != 2 or self.W2.shape[0] != self.W2.shape[1]:
            raise ValueError(f"W2 must be (D,D), got {self.W2.shape}")
        if self.W4.shape[-1] != self.W2.shape[0]:
            raise ValueError(f"W4 last dim {self.W4.shape} must match D={self.W2.shape[0]}")

    @property
    def dim(self):
        return self.W2.shape[0]

    @classmethod
    def from_npz(cls, path):
        """Load the head from a numpy .npz (keys W2/W4/b4/gamma/beta) — the form
        baked into the runtime image so it needs only numpy, not safetensors."""
        z = np.load(path)
        return cls(
            w2=z["W2"],
            w4=z["W4"],
            b4=z["b4"] if "b4" in z else None,
            gamma=z["gamma"] if "gamma" in z else None,
            beta=z["beta"] if "beta" in z else None,
        )

    def to_npz(self, path):
        """Save the head as a numpy .npz (the runtime form; built in the image's
        modelprep stage so the slim runtime needs only numpy)."""
        d = {"W2": self.W2, "W4": self.W4, "b4": np.asarray(self.b4, dtype=np.float32)}
        if self.gamma is not None and self.beta is not None:
            d["gamma"] = self.gamma
            d["beta"] = self.beta
        np.savez(path, **d)

    @classmethod
    def from_dir(cls, model_dir):
        """Load the head from `model_dir`. Prefers a prebuilt `head.npz` (numpy-only,
        baked into the image); else the sentence-transformers
        2_Dense/3_LayerNorm/4_Dense safetensors (needs the safetensors lib)."""
        npz = os.path.join(model_dir, "head.npz")
        if os.path.exists(npz):
            return cls.from_npz(npz)
        from safetensors.numpy import load_file

        d2 = load_file(os.path.join(model_dir, "2_Dense", "model.safetensors"))
        d4 = load_file(os.path.join(model_dir, "4_Dense", "model.safetensors"))
        ln_path = os.path.join(model_dir, "3_LayerNorm", "model.safetensors")
        ln = load_file(ln_path) if os.path.exists(ln_path) else {}
        return cls(
            w2=d2["linear.weight"],
            w4=d4["linear.weight"],
            b4=d4.get("linear.bias"),
            gamma=ln.get("norm.weight"),
            beta=ln.get("norm.bias"),
        )

    def score(self, encoder_vec):
        """Score one CLS vector (shape (D,)) or a batch (shape (N,D)). Returns a float
        or a (N,) float array."""
        v = np.asarray(encoder_vec, dtype=np.float32)
        single = v.ndim == 1
        if single:
            v = v[None, :]
        if v.shape[-1] != self.dim:
            raise ValueError(f"encoder vec dim {v.shape[-1]} != head dim {self.dim}")
        h = gelu(v @ self.W2.T)
        if self.gamma is not None and self.beta is not None:
            h = layer_norm(h, self.gamma, self.beta)
        s = h @ self.W4.T + self.b4  # (N,1)
        s = s.reshape(-1)
        return float(s[0]) if single else s

    def rerank(self, query, documents, embed_pairs, sep="</s>"):
        """Rerank `documents` for `query`. `embed_pairs(list[str]) -> np.ndarray (N,D)`
        is the caller's encoder embed function (the llama.cpp `/v1/embeddings` CLS call
        over `query<sep>doc`). Returns results sorted by descending score:
        [{"index": i, "relevance_score": s}, ...]."""
        if not documents:
            return []
        texts = [f"{query}{sep}{doc}" for doc in documents]
        vecs = np.asarray(embed_pairs(texts), dtype=np.float32)
        if vecs.ndim == 1:
            vecs = vecs[None, :]
        scores = self.score(vecs)
        scores = np.atleast_1d(scores)
        order = np.argsort(-scores)
        return [{"index": int(i), "relevance_score": float(scores[i])} for i in order]
