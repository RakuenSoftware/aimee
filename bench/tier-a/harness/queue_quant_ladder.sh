#!/bin/bash
# The quant ladder, queued behind the running v8 baseline so the 5080 never idles.
#
# Order is the operator's, and it is a priority order rather than a tidy one: the
# E2B Q4-vs-Q6 pair first, because that is the decision waiting on evidence, then
# E4B Q6, then the Q8 ceiling for both.
#
#   (running)  E4B Q4   } the v8 baseline
#   (running)  E2B Q4   }
#              E2B Q6   <- decides E2B's quant
#              E4B Q6   <- the current default for E4B
#              E2B Q8   } does the curve keep climbing, or stop
#              E4B Q8   }
#
# Every arm: corpus v5, prompt v8, thinking on, -c 8192, --no-mmproj, the same
# CUDA build on the same card, one at a time. The only variable is the weights.
#
# Why this is worth 4 hours: the standing quant decision was made on 69 notes,
# and the E2B half of it was an INDISTINGUISHABLE +0.0012 [-0.0633, +0.0690]
# taken on a lane that defect 30 later showed was serving from an 8GB iGPU. E2B's
# quant choice currently rests on nothing.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}

# label|gguf, in the order requested
LADDER="\
E2B.UD-Q6_K_XL.v8|/opt/hf/e2b-q6.gguf
E4B.UD-Q6_K_XL.v8|/opt/hf/e4b-q6.gguf
E2B.UD-Q8_K_XL.v8|/opt/hf/e2b-q8.gguf
E4B.UD-Q8_K_XL.v8|/opt/hf/e4b-q8.gguf"

echo "[queue] waiting for the v8 baseline to finish..."
until grep -q "BASELINE DONE" "$OUT/driver.log" 2>/dev/null; do sleep 60; done
echo "[queue] baseline done; starting the ladder"

while IFS='|' read -r label model; do
  [ -n "${label:-}" ] || continue
  echo "[queue] -> $label"
  MODEL="$model" LABEL="$label" GOLD="$GOLD" OUT="$OUT" bash harness/sweep_quant_arm.sh
done <<< "$LADDER"
echo "[queue] === LADDER COMPLETE ==="
