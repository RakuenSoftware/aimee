#!/bin/bash
# Fetch the gemma-4 MTP draft heads for speculative decoding.
# Q8_0 for both: the draft head is small, and a low-quality draft lowers the
# acceptance rate, which is the thing that decides whether speculation pays.
pct exec 140 -- bash -lc '
  cd /opt/hf
  for m in E2B E4B; do
    f="mtp-gemma-4-${m}-it-Q8_0.gguf"
    [ -s "$f" ] && { echo "have $f"; continue; }
    url="https://huggingface.co/ggml-org/gemma-4-${m}-it-GGUF/resolve/main/${f}?download=true"
    curl -sfL -o "${f}.part" "$url" && mv "${f}.part" "$f" && echo "fetched $f" \
      || echo "FAILED $f"
  done
  ls -la /opt/hf/mtp-*.gguf 2>/dev/null
'
