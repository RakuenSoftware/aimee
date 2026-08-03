# Debian 13 (trixie) for libpq 17. The kb links db2/vault_operator_status_runtime.c,
# which uses the PostgreSQL 17 async-cancel API (PQcancelCreate / PGcancelConn /
# PQcancelPoll). Bookworm ships libpq 15, where those symbols do not exist, so the
# kb build failed here with -Werror=implicit-function-declaration while the native
# CI build stayed green -- `make all server` does not compile KB_SRCS, so this
# image was the only place that file was ever built.
# Global build args, declared before the first FROM so every stage can use them.
ARG PG_MAJOR=18
ARG PGVECTORSCALE_VERSION=0.9.0

FROM debian:trixie-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        git \
        libp11-kit-dev \
        libpq-dev \
        libssl-dev \
        libzstd-dev \
        pkg-config \
        python3 \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
ARG AIMEE_VERSION=""
# Ship the tree-sitter extraction front-end: fetch + sha256-verify the pinned
# grammars (scripts/fetch-treesitter.sh; git + network needed only in this trusted
# build stage), then build the kb with AIMEE_TREESITTER=1 (real-AST C/C++ class/
# method extraction). See docs/proposals/pending/cpp-class-method-extraction.md.
RUN sh scripts/fetch-treesitter.sh \
    && make -C src ../aimee-kb -j"$(nproc)" AIMEE_TREESITTER=1 ${AIMEE_VERSION:+GIT_VERSION=v$AIMEE_VERSION}

RUN python3 scripts/export_c_repositories.py --runtime-bundle /module-runtime \
    && mkdir -p /module-runtime/bin \
    && for source in /module-runtime/src/*.c; do \
         binary="${source##*/}"; binary="${binary%.c}"; \
         cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc/core/event_bus/include \
           -Isrc/modules/memory/include -Isrc/modules/learning/include \
           -Isrc/modules/routing/include -Isrc/modules/delegates/include \
           -Isrc/modules/tools/include -Isrc/modules/workspace/include \
           -Isrc/modules/git/include -Isrc/modules/skills/include \
           -Isrc/modules/response-composition/include \
           "$source" \
           src/core/event_bus/bus_attach.c src/core/event_bus/bus_client.c \
           src/core/event_bus/bus_endpoint.c src/core/event_bus/bus_region.c \
           src/core/event_bus/bus_ring.c src/core/event_bus/bus_wire.c \
           src/core/event_bus/module_protocol.c src/core/event_bus/module_runtime.c \
           -pthread \
           -o "/module-runtime/bin/$binary"; \
       done

# pgvectorscale (StreamingDiskANN). Always built: it adds ~1 MB to the image, and
# the kb already decides at RUNTIME whether to use it -- pgvec_vectorscale_available()
# probes pg_extension and falls back to HNSW with a warning when it is absent
# (src/db2/pgvec_transport.c). Gating it at build time would defeat that and make
# the index type a property of which image you happened to pull.
#
# The Rust toolchain and pgrx live only in this stage; the runtime image receives
# the built extension files and none of the build chain. CI caches this layer
# (cache-from/to type=gha,mode=max), so it recompiles only when the pins below move.
FROM debian:trixie-slim AS pgvectorscale-build
ARG PG_MAJOR
ARG PGVECTORSCALE_VERSION
# Same retry as the runtime stage: one transient TLS reset here fails the build.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
    && install -d /usr/share/postgresql-common/pgdg \
    && for a in 1 2 3 4 5; do \
         curl -fsS --connect-timeout 10 --max-time 60 \
           -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
           https://www.postgresql.org/media/keys/ACCC4CF8.asc && break; \
         echo "pgdg key fetch failed (attempt $a/5); backing off"; sleep $((a * 5)); \
       done \
    && test -s /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
    && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] http://apt.postgresql.org/pub/repos/apt trixie-pgdg main" \
        > /etc/apt/sources.list.d/pgdg.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential clang git pkg-config libssl-dev \
        "postgresql-${PG_MAJOR}" "postgresql-server-dev-${PG_MAJOR}" \
    && rm -rf /var/lib/apt/lists/*
ENV CARGO_HOME=/usr/local/cargo
ENV PATH=/usr/local/cargo/bin:$PATH
# cargo-pgrx must match the pgrx the crate depends on, so it is read from the
# checkout's Cargo.toml rather than pinned separately here.
RUN curl -fsS https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable \
    && git clone --depth 1 --branch "${PGVECTORSCALE_VERSION}" \
        https://github.com/timescale/pgvectorscale.git /src/pgvectorscale \
    && cd /src/pgvectorscale/pgvectorscale \
    && pgrx_version="$(awk -F'"' '/^pgrx[[:space:]]*=/{print $2; exit}' Cargo.toml)" \
    && echo "building pgvectorscale ${PGVECTORSCALE_VERSION} against pgrx ${pgrx_version}" \
    && cargo install --locked cargo-pgrx --version "${pgrx_version}" \
    && cargo pgrx init "--pg${PG_MAJOR}=/usr/lib/postgresql/${PG_MAJOR}/bin/pg_config" \
    && cargo pgrx install --release --pg-config "/usr/lib/postgresql/${PG_MAJOR}/bin/pg_config"
# Collect only the installed extension artifacts, at the paths the runtime uses.
RUN mkdir -p "/pgvectorscale/usr/lib/postgresql/${PG_MAJOR}/lib" \
        "/pgvectorscale/usr/share/postgresql/${PG_MAJOR}/extension" \
    && cp "/usr/lib/postgresql/${PG_MAJOR}/lib/vectorscale"*.so \
        "/pgvectorscale/usr/lib/postgresql/${PG_MAJOR}/lib/" \
    && cp "/usr/share/postgresql/${PG_MAJOR}/extension/vectorscale"* \
        "/pgvectorscale/usr/share/postgresql/${PG_MAJOR}/extension/"

# Runtime must match the build stage: libpq5 here has to provide at least the
# PostgreSQL 17 symbols the kb was linked against (PGDG's libpq 18 does).
FROM debian:trixie-slim

# python3 runs the sidecar clients the kb popens (llm-chat.py,
# learning-synthesize.py, curator-extract.py -> LLM endpoint) AND the in-container
# embedder below, which needs pip to install torch.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libgomp1 \
        libpq5 \
        libssl3 \
        libzstd1 \
        python3 \
        python3-pip \
        python3-venv \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

# ---- the in-container embedder ------------------------------------------------
# The kb embeds ITSELF now. There is no embedder sidecar and no aimee-llm hop on the
# retrieval path: embedder-server.py runs on loopback inside this container with the
# weights BAKED IN, so a fresh container embeds immediately with no model download and
# no network at all.
#
# This deliberately reverses the split the unified-llm cutover introduced. That design
# bought a model-less kb image at the cost of a second container, a supervisor role, a
# gateway, an HTTP hop and a per-role env matrix — all to serve an embedder small enough
# to bake. With the reranker gone (measured negative) the only thing left on that path
# was embedding, so the container is retired and its per-model facts move back to the one
# place that reads them: scripts/embedders.json.
#
# CPU-only torch: the CUDA wheels are ~2GB of accelerator runtime this image never uses.
#
# AIMEE_EMBEDDER=none SKIPS ALL OF THIS. A deployment pointing EMBEDDER_URL at an
# external endpoint still carried CPU torch, sentence-transformers and a baked model
# it never loads: roughly a gigabyte of image to hold code that never executes. That
# variant could not be expressed before, and it is the third kb tag now.
#
# A conditional RUN body rather than a conditional stage. The weights land in the
# final image either way; there is nothing to select between, only work to skip.
# The default matches the one on the bake ARG below, and it is NOT optional: `set -u`
# aborts on an unset variable, so declaring this bare made every build that does not
# pass --build-arg AIMEE_EMBEDDER fail with
#   /bin/sh: 1: AIMEE_EMBEDDER: parameter not set
# The publish workflows always pass it; compose builds (and the e2e-docker tests that
# use them) do not, which is why building locally never reproduced it.
#
# bekko rather than `none`, so a build with no build-arg behaves exactly as it did
# before this change. `none` is opt-in.
ARG AIMEE_EMBEDDER=bekko-a25m
ENV EMBEDDER_VENV=/opt/aimee/embedder-venv
RUN set -eux; \
    if [ "$AIMEE_EMBEDDER" = "none" ]; then \
      echo "AIMEE_EMBEDDER=none: no in-container embedder is baked;" \
           "EMBEDDER_URL must be set at runtime"; \
    else \
      python3 -m venv "$EMBEDDER_VENV"; \
      "$EMBEDDER_VENV/bin/pip" install --no-cache-dir --quiet \
          --index-url https://download.pytorch.org/whl/cpu torch; \
      "$EMBEDDER_VENV/bin/pip" install --no-cache-dir --quiet \
          "sentence-transformers>=3.3" "transformers>=5.2" einops; \
      find "$EMBEDDER_VENV" -name '__pycache__' -type d -prune -exec rm -rf {} +; \
      rm -rf /root/.cache/pip; \
    fi

# DB2 engine, from PGDG rather than Debian: trixie ships PostgreSQL 17 with
# pgvector 0.8.0, and its pgvector hard-depends on postgresql-17-jit-llvm, which
# drags LLVM into the runtime image. PGDG carries PostgreSQL 18 (the current
# stable major) with pgvector 0.8.5 depending only on the server and libc.
# PostgreSQL 18 is also the version pgvectorscale builds against.
ARG PG_MAJOR=18
# The PGDG key fetch is retried: it is a single point of build failure on a
# network hiccup and this layer runs on every kb build. Seen failing CI with
# "curl: (56) OpenSSL SSL_read: unexpected eof while reading".
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && install -d /usr/share/postgresql-common/pgdg \
    && for a in 1 2 3 4 5; do \
         curl -fsS --connect-timeout 10 --max-time 60 \
           -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
           https://www.postgresql.org/media/keys/ACCC4CF8.asc && break; \
         echo "pgdg key fetch failed (attempt $a/5); backing off"; sleep $((a * 5)); \
       done \
    && test -s /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
    && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] http://apt.postgresql.org/pub/repos/apt trixie-pgdg main" \
        > /etc/apt/sources.list.d/pgdg.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        "postgresql-${PG_MAJOR}" \
        "postgresql-${PG_MAJOR}-pgvector" \
    && rm -rf /var/lib/apt/lists/*
ENV AIMEE_DB2_PG_MAJOR=${PG_MAJOR}

# The vectorscale extension files, built above. Whether the kb actually creates
# diskann indexes stays a runtime decision (context.kb index type + corpus size).
COPY --from=pgvectorscale-build /pgvectorscale/ /

ENV AIMEE_HOME=/var/lib/aimee
# DB2 is unset on purpose. Unset means "the operator configured nothing", and the
# entrypoint then runs the in-image PostgreSQL 18 + pgvector cluster under
# $AIMEE_HOME/postgres. Setting it to any URL selects an external server and the
# entrypoint starts nothing — that path is fully supported and unchanged.
#
# This used to default to postgresql://aimee:aimee@postgres:5432/aimee_shared,
# which made "nothing configured" indistinguishable from "use the sibling
# container" and left a bare `docker run` pointed at a host that does not exist.
# New-install Compose manifests leave AIMEE_DB2_URL unset so the KB owns this
# internal cluster. Operators can still set the variable to select an external DB2.
# No baked embedder/LLM endpoint defaults. The kb runs NO model runtime; it calls
# an external aimee-llm container (CPU/GPU) or endpoint. Point it with ONE of:
#   SYNTHESIS_ENDPOINT       unified container -> embed + synth (one knob)
#   EMBEDDER_URL  pin the embedder (/embed) independently
#   SYNTHESIS_ENDPOINT        Tier-A synth only (small-model interface)
# One of these is REQUIRED. The seeded default config selects the remote embedder
# sidecar, so with no endpoint set the DB2 dim probe fails and db2_init refuses to
# initialise rather than record a wrong vector width (db2_init.c) -- the kb then
# never binds its HTTP port. Verified on the shipped image with nothing else
# configured: the embedded cluster comes up, the service does not.
#
# The 384-dim builtin embedder exists but is NOT reached in that state: it applies
# only when no embed command is configured at all, and the baked config always
# configures one. The deploy unit (compose / smoothnas plugin) sets these and
# brings up a default CPU aimee-llm sibling, which is why this is invisible there. (Old combined leftovers
# EMBEDDER_URL=embedder:8080 / SYNTHESIS_ENDPOINT=llm:8080 were removed: on a
# split deploy they silently pointed at non-existent services.)
# Bind the /v1 HTTP API on 0.0.0.0 (not the 127.0.0.1 default), so the published
# port and container-IP access reach it from outside the container.
ENV AIMEE_KB_HTTP_BIND=1

RUN useradd --system --home-dir /var/lib/aimee --create-home --shell /usr/sbin/nologin aimee
# The synthesis identity directory must exist IN THE IMAGE, owned by the runtime
# user, because the sidecar deployment mounts a named volume over this path.
#
# Docker initialises an empty named volume from whatever the image has at the mount
# point, ownership included -- but if the path does not exist in the image it creates
# the volume root-owned, and the kb (which runs as `aimee`) cannot write into it. That
# is not theoretical: booting this produced
#   kb_synthesis_identity: cannot create .../synthesis-tls/server.pem: Permission denied
# and the sidecar then refuses to start, because it has no identity to present. The
# unit test missed it by creating the directory as the same user it then wrote as.
RUN install -d -o aimee -g aimee -m 0700 /var/lib/aimee/synthesis-tls
COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb
COPY --from=build /src/aimee-kb-resolver /usr/local/bin/aimee-kb-resolver
COPY --from=build /module-runtime/bin/ /usr/local/libexec/aimee-modules/
COPY --from=build /module-runtime/grants/kb/ /opt/aimee/module-grants/kb/
COPY --from=build /module-runtime/kb.modules /opt/aimee/module-grants/kb.modules

# The embedder registry: every per-model fact that changes the vectors (pooling, width,
# context, prefixes), keyed by model identity. Read by the in-container embedder to know
# what to load and which prefixes to apply, and by the server for the wizard's picker.
COPY scripts/embedders.json /opt/aimee/embedders.json
COPY scripts/embedder-server.py /opt/aimee/scripts/embedder-server.py

# Bake the weights for the ONE embedder this image variant serves, so a fresh
# container embeds with no download and an air-gapped install works. Pinned to the
# registry's revision: a floating ref would let a rebuild change the vector space
# silently.
#
# AIMEE_EMBEDDER selects the registry entry. THREE images come out of this file,
# and the embedder is now the only axis:
#
#   AIMEE_EMBEDDER            image            notes
#   none                      aimee-kb         no torch, no weights; EMBEDDER_URL required
#   bekko-a25m (384)          aimee-kb-a25m
#   nomic-embed-text-v2-moe   aimee-kb-nomic
#
# Synthesis used to be a second axis here, doubling the matrix. It is its own image
# now (aimee-llm-e2b / -e4b) deployed beside this one, because llama.cpp and a
# multi-gigabyte GGUF have no business being rebuilt every time kb code changes.
#
# The registry lists both models; only the selected one is fetched. Baking both
# would put nomic's ~1.8GB into every image, which is why these are separate tags.
#
# THE EMBEDDER AXIS IS A ONE-WAY DOOR PER INSTALL. DB2 records the vector-column
# width and refuses startup on drift, so an install embedded with bekko (384)
# cannot be moved to a nomic image (768) without a full reindex. Do not offer this
# as a runtime switch; it is a deployment-time choice with data consequences.
#
# A deployment that wants a different embedder again points EMBEDDER_URL at its own
# endpoint rather than baking a third model in here.
ARG AIMEE_EMBEDDER=bekko-a25m
#
# FETCH ONLY WHAT THE TORCH PATH LOADS. snapshot_download takes the whole repo by
# default, and a repo may publish the same weights several times over: bekko ships an
# onnx/ tree (nine variants, ~2.2GB) and an openvino/ tree next to its safetensors, none
# of which sentence-transformers touches. Baking them cost 3.4GB of image for nothing.
# Excluding the alternate runtimes and the legacy .bin duplicates keeps this to the
# safetensors + tokenizer the loader actually reads.
# The baked cache is READ-ONLY at runtime (built as root, served as `aimee`), but
# trust_remote_code compiles the fetched modelling code into a writable module cache. It
# defaults inside HF_HOME, which the non-root user cannot write — nomic then failed with
# "Permission denied: /opt/aimee/models/modules". Point that one writable path at the
# home volume and leave the weights immutable.
ENV HF_HOME=/opt/aimee/models \
    HF_MODULES_CACHE=/var/lib/aimee/.cache/huggingface/modules \
    HF_HUB_OFFLINE=1
# Skipped on AIMEE_EMBEDDER=none, which has no venv to run this with: the interpreter
# lives in the venv the step above did not create.
#
# The SKIP IS THE INTERPRETER, not an `if` around the heredoc. Docker ends a RUN at
# the heredoc terminator, so a trailing `fi` after PYBAKE is parsed as a Dockerfile
# instruction and the build dies with "unknown instruction: fi". Choosing /bin/true
# before the heredoc keeps this one instruction: the shell still feeds it the script,
# and it discards it.
RUN --mount=type=cache,target=/root/.cache/huggingface \
    if [ "$AIMEE_EMBEDDER" = "none" ]; then \
      echo "AIMEE_EMBEDDER=none: no weights baked"; BAKE=/bin/true; \
    else \
      BAKE="$EMBEDDER_VENV/bin/python"; \
    fi; \
    AIMEE_EMBEDDER="$AIMEE_EMBEDDER" \
    HF_HUB_OFFLINE=0 "$BAKE" - <<'PYBAKE'
import json, os, sys
from huggingface_hub import snapshot_download
with open("/opt/aimee/embedders.json", encoding="utf-8") as handle:
    table = json.load(handle)["embedders"]

# Bake exactly the selected entry. An unknown name is a BUILD failure, not a
# silently-empty image: a container with no baked weights starts fine and then
# cannot embed, which surfaces as a retrieval outage rather than a build error.
selected = os.environ.get("AIMEE_EMBEDDER", "").strip()
if selected not in table:
    sys.exit(f"AIMEE_EMBEDDER={selected!r} is not in the registry. "
             f"Known: {', '.join(sorted(table))}")
table = {selected: table[selected]}
SKIP = [
    "onnx/*", "openvino/*", "*.onnx", "*.onnx_data",   # alternate runtimes
    "*.bin", "*.h5", "*.msgpack", "*.tflite", "*.ckpt",  # duplicate/legacy formats
    "*.gguf",                                          # not this runtime either
]
def code_repos(snapshot_dir):
    """Repos holding this model's custom modelling code, from config.json auto_map.

    A trust_remote_code model does not necessarily carry its own code — auto_map may point
    every class at a SEPARATE repo. Downloading only the weights then leaves the loader
    reaching for the Hub at first use, which fails closed under HF_HUB_OFFLINE, defeating
    the point of baking. So follow the references.

    The bundled embedder needs none of this (ModernBERT is native to transformers), but a
    swapped-in model may, and the failure is a container that starts and cannot embed.
    """
    import os
    out = set()
    cfg = os.path.join(snapshot_dir, "config.json")
    if not os.path.exists(cfg):
        return out
    with open(cfg, encoding="utf-8") as handle:
        auto_map = (json.load(handle).get("auto_map") or {})
    for target in auto_map.values():
        if isinstance(target, str) and "--" in target:
            out.add(target.split("--", 1)[0])
        elif isinstance(target, (list, tuple)):
            for item in target:
                if isinstance(item, str) and "--" in item:
                    out.add(item.split("--", 1)[0])
    return out


for name, spec in table.items():
    repo, revision = spec["repo"], spec.get("revision") or "main"
    print(f"baking {name}: {repo}@{revision}", flush=True)
    local = snapshot_download(repo, revision=revision, ignore_patterns=SKIP)
    # Code repos are referenced by name only — auto_map carries no revision — so these
    # take the default branch. That is a looser pin than the weights get; a model whose
    # code must be pinned needs its auto_map repo added to the registry explicitly.
    for code_repo in sorted(code_repos(local)):
        print(f"  + code: {code_repo}", flush=True)
        snapshot_download(code_repo, allow_patterns=["*.py", "*.json", "*.txt"])
PYBAKE

# The bake runs as root and huggingface_hub writes its tree-cache metadata 0600, so
# the `aimee` runtime user cannot read it. The weights themselves land world-readable,
# so the model still loads — it just logs "Ignoring corrupted tree cache file …
# Permission denied" on every start and re-walks what the cache existed to avoid.
# a+rX: readable everywhere, traversable on directories, and no file gains +x.
# The directory does not exist on AIMEE_EMBEDDER=none, and chmod -R on a missing
# path is an error rather than a no-op.
RUN if [ -d /opt/aimee/models ]; then chmod -R a+rX /opt/aimee/models; fi

# NO BUNDLED SYNTHESIS. llama.cpp and its GGUF used to be baked here, which coupled a
# multi-gigabyte, near-static artefact to the image rebuilt on every kb code change:
# three kb tags under three cache scopes each rebuilding the LTO C binary, postgres,
# pgvectorscale and torch, and pushing ~10 GB twice, for one code change.
#
# They now live in aimee-llm-e{2,4}b, deployed beside this container and reached over
# mTLS. The weights are still baked -- into the image whose inputs change on the order
# of never -- so the reason they were baked is intact. The kb resolves
# SYNTHESIS_ENDPOINT exactly as it does for any external provider; it no longer cares
# whether the model is on the same host.

# Sidecar clients (the LLM access code the kb invokes via popen).
COPY scripts/embed-remote.py scripts/llm-chat.py \
     scripts/learning-synthesize.py scripts/curator-extract.py scripts/llm-rewrite.py \
     scripts/guardrails-semantic.py /opt/aimee/scripts/
# Baked default config: selects the sidecar commands (endpoints come from env).
# Kept OUTSIDE $AIMEE_HOME so a bind mount over /var/lib/aimee can't shadow it;
# the entrypoint seeds it into $AIMEE_HOME/.config/aimee on first start.
COPY deploy/container/aimee.yaml /opt/aimee/defaults/aimee.yaml
COPY deploy/container/aimee-kb-entrypoint.sh /usr/local/bin/aimee-kb-entrypoint.sh
COPY deploy/container/module-supervisor.sh /usr/local/bin/module-supervisor.sh
COPY deploy/container/aimee-kb-db-export.sh /usr/local/bin/aimee-kb-db-export
RUN chmod +x /usr/local/bin/aimee-kb-entrypoint.sh /usr/local/bin/aimee-kb-db-export \
    /usr/local/bin/module-supervisor.sh

USER aimee
WORKDIR /var/lib/aimee
EXPOSE 8741

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8741/v1/health >/dev/null || exit 1

# The entrypoint raises the worker-thread stack rlimit and seeds the baked
# config into $AIMEE_HOME when a bind mount has shadowed the image copy, then
# execs aimee-kb. See deploy/container/aimee-kb-entrypoint.sh.
ENTRYPOINT ["/usr/local/bin/aimee-kb-entrypoint.sh"]
# aimee-kb serves the /v1 HTTP API only; --http-port is required (it exits without
# one). The legacy --socket transport was retired (#2747).
CMD ["--http-port=8741"]
