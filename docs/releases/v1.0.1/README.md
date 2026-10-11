# Aimee 1.0.1: signed model plugins

This regular application release includes five model plugin bundles. Plugin version **0.3.3**
remains independent of application version **1.0.1**. There is one file per supported model;
each contains that model's adapter, shared-runtime build 5, the pinned GGUF loader and their
existing verified Ed25519 signatures. No separate plugin release is needed.

| Model | Signed plugin bundle | SHA-256 of bundle |
| --- | --- | --- |
| Gemma4 E2B | [aimee-gemma4-e2b-0.3.3.zip](plugins/aimee-gemma4-e2b-0.3.3.zip) | `9985d496c9c764933f4d27304411546d379a0f9e26d72a9f051b4053230380c7` |
| Gemma4 E4B | [aimee-gemma4-e4b-0.3.3.zip](plugins/aimee-gemma4-e4b-0.3.3.zip) | `1903807877d9615a67c34786a1f348a45db6cf0fd5fc42f0c3158afc32f6bcc9` |
| Gemma4 12B | [aimee-gemma4-12b-0.3.3.zip](plugins/aimee-gemma4-12b-0.3.3.zip) | `79952b1ab9aac6c88b95b66ca6e9b41454a8b2a61ca9ed210c289340700fea73` |
| Gemma4 26B A4B | [aimee-gemma4-26b-0.3.3.zip](plugins/aimee-gemma4-26b-0.3.3.zip) | `699fc65cff8691dcb6ee3b2c5cc12dd147c4a1404f54f8ddab616affa1b0d68e` |
| Qwen3.8 27B | [aimee-qwen3-8-27b-0.3.3.zip](plugins/aimee-qwen3-8-27b-0.3.3.zip) | `ee0dd941162728d8d014b71e53f8db727f124beab491582c84c80914f89a18c2` |

The ZIP container groups the signed payload; the signatures authenticate its three wheels,
manifest and checksum file. Wheel bytes are unchanged from the qualified 0.3.3 build. The signed
manifest retains the original candidate identity and historical URL fields; installation below
uses the files in the bundle and does not use those URLs.

## Verify and install your model

Download one bundle from the table, extract it into an empty directory and verify it before
installation. These commands require Linux, Bash, Python 3 and OpenSSL. Replace the filename
with your selected model's bundle.

```sh
set -euo pipefail
mkdir -p native-memory-0.3.3
python3 -m zipfile -e aimee-gemma4-12b-0.3.3.zip native-memory-0.3.3
cd native-memory-0.3.3
python3 - <<'KEY'
from pathlib import Path
Path('public.der').write_bytes(bytes.fromhex(
    '302a300506032b6570032100'
    '5d14ca247841adcc1130a4d95b82ead12ba699c03f2ba45f2f47524abde0c6c2'))
KEY
for asset in *.whl manifest.json SHA256SUMS; do
  openssl pkeyutl -verify -pubin -keyform DER -inkey public.der \
    -rawin -in "$asset" -sigfile "$asset.sig"
done
sha256sum --ignore-missing --check SHA256SUMS
```

The checksum file covers the full original seven-wheel candidate; each bundle intentionally
contains only its selected model and two dependencies. All three supplied wheels and both metadata
files must pass signature verification. The public key above is pinned; do not replace it with
a key downloaded alongside the bundle.

Prepare CPython 3.12, glibc 2.39+ and the correct GPU-specific vLLM 0.30.0 environment first,
then install the three verified wheels:

```sh
python -m pip install --no-index --no-deps ./*.whl
python -m pip check
```

Use a separate Python environment for each serving instance. Install only the selected model's
bundle in that environment. Model weights, GPU runtime and Aimee application binaries are separate.
See the [plugin guide](../../NATIVE_MEMORY_PLUGIN.md) for enrollment, configuration and serving.
The [qualification record](../native-memory-v0.3.3/qualification/README.md) describes the five
bounded model smokes; repackaging does not broaden that qualification.
