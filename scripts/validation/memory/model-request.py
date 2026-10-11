#!/usr/bin/env python3
"""One fixture request to an existing mTLS model; never posts to Discord."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--tls-directory", type=Path, required=True)
    parser.add_argument("--server-name", default="aimee-llm")
    parser.add_argument("--operation", choices=("models", "chat", "embed", "native-embed", "embedding-health"), default="chat")
    options = parser.parse_args()
    endpoint = urlsplit(options.endpoint)
    if endpoint.scheme != "https" or endpoint.username or endpoint.query or endpoint.fragment:
        parser.error("an HTTPS model origin is required")
    port = endpoint.port or 443
    route = {"models": "/v1/models", "chat": "/v1/chat/completions", "embed": "/v1/embeddings", "native-embed": "/embed_batch?input_type=document", "embedding-health": "/health"}[options.operation]
    command = ["curl", "--silent", "--show-error", "--fail", "--max-time", "180", "--noproxy", "*",
               "--resolve", f"{options.server_name}:{port}:{endpoint.hostname}",
               "--cacert", str(options.tls_directory / "ca.pem"),
               "--cert", str(options.tls_directory / "client.pem"),
               "--key", str(options.tls_directory / "client.key"),
               "--header", "Content-Type: application/json"]
    body = None
    if options.operation not in ("models", "embedding-health"):
        body = sys.stdin.buffer.read(1_048_577)
        if len(body) > 1_048_576:
            parser.error("fixture payload too large")
        request = json.loads(body)
        if options.operation == "native-embed":
            texts = request["input"]
            if isinstance(texts, str): texts = [texts]
            if not isinstance(texts, list) or not all(isinstance(x, str) for x in texts):
                parser.error("string embedding inputs required")
            body = json.dumps(texts).encode()
        command += ["--data-binary", "@-"]
    command += [f"https://{options.server_name}:{port}{route}"]
    result = subprocess.run(command, input=body, capture_output=True, timeout=190)
    if result.returncode:
        print(json.dumps({"error": "model request failed", "transport_exit": result.returncode}))
        return 1
    value = json.loads(result.stdout)
    if options.operation == "native-embed":
        if not isinstance(value, list): raise RuntimeError("native embed response must be aligned vectors")
        value = {"object": "list", "model": request.get("model", "native"), "data": [
            {"object": "embedding", "index": i, "embedding": vector} for i, vector in enumerate(value)],
            "usage": {"prompt_tokens": 0, "total_tokens": 0}}
    print(json.dumps(value, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
