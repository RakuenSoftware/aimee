# Discord webhook and Gemma4 E2B chatbot

This optional channel bridge posts through a configured Discord incoming webhook and receives
bot mentions through a Discord application. Gemma4 E2B supplies the reply. The CPU
bridge archives exchanges and captures human statements through Aimee’s durable
fact pipeline; the separate GPU setup below uses the enrolled native-memory plugin.
The bot uses a dedicated Aimee environment and does not run agent tools.

An incoming webhook can post messages but cannot read channel chat. Two-way chat therefore needs
a Discord application bot token as well as the webhook. The bridge requests guild message events,
responds only to explicit mentions in the configured channel or its threads, and ignores bots,
webhooks and DMs. It does not request privileged Message Content, member or presence intents.
Discord supplies the content of messages that mention the bot.

Official references: [Discord webhooks](https://docs.discord.com/developers/platform/webhooks),
[message content access](https://docs.discord.com/developers/events/gateway#message-content-intent),
and [discord.py](https://discordpy.readthedocs.io/en/stable/api.html).

## Prepare the Discord service

Use a Linux account on the inference host. Keep credentials outside the repository. Store the
webhook URL in `~/.config/aimee-discord/webhook.url` and the application bot token in
`~/.config/aimee-discord/bot.token`, each as one line with mode `0600`; the containing directory
must have mode `0700`. The bridge never logs these values or raw Discord/API exceptions.

Create the Discord application and invite its bot to the intended server with View Channel
permission in the selected channel. Reply delivery uses the supplied webhook. No Administrator,
Manage Server, member-list or privileged message-content permission is needed. Threads require
access to the intended thread. Do not enable unrelated bot permissions.

```sh
mkdir -p ~/.local/share/aimee-discord ~/.config/aimee-discord
chmod 700 ~/.local/share/aimee-discord ~/.config/aimee-discord
cp integrations/discord/{bridge.py,prepare_e2b.py,serve_e2b.py,requirements.txt} \
  ~/.local/share/aimee-discord/
python3 -m venv ~/.local/share/aimee-discord/venv
~/.local/share/aimee-discord/venv/bin/python -m pip install \
  -r ~/.local/share/aimee-discord/requirements.txt
cp integrations/discord/config.example.json ~/.config/aimee-discord/config.json
chmod 600 ~/.config/aimee-discord/config.json
```

Edit `config.json` with the absolute credential paths, server ID, channel ID and the model's exact
served name. An empty `allowed_user_ids` list permits humans who can mention the bot in the
configured channel; populate it to restrict use to specific Discord users. The service verifies
that the webhook belongs to this exact server and channel before receiving turns.

Check configuration and the webhook without posting a message:

```sh
~/.local/share/aimee-discord/venv/bin/python ~/.local/share/aimee-discord/bridge.py \
  --config ~/.config/aimee-discord/config.json --mode check-config
~/.local/share/aimee-discord/venv/bin/python ~/.local/share/aimee-discord/bridge.py \
  --config ~/.config/aimee-discord/config.json --mode check-webhook
```

For outbound-only delivery, feed an operator-selected message through stdin with
`--mode send-stdin`. That mode posts to the configured channel; configuration checks do not post.
On its first Discord ready event after each process start, the bot posts a short
“Systems initializing… Aimee is online” announcement in the configured webhook channel.
Gateway reconnects do not repeat it. A failed delivery is logged without stopping the chatbot
and is not automatically retried, to avoid duplicate announcements.

All webhook messages disable user, role and everyone mentions, even when model output contains
Discord mention markup. Long replies are split within Discord's message limit.

## CPU-only Aimee and E2B deployment

For CPU inference, use the published `aimee-llm-e2b:1.0.0` image, which includes
portable llama.cpp and its baked E2B QAT checkpoint. This is the standard Aimee
synthesis service. It does not use the GPU-qualified native-memory vLLM plugin.
A CPU deployment does not establish native-memory plugin qualification.

On an unprivileged Debian 13 LXC, enable Docker nesting, allocate 12 CPU cores,
24 GiB RAM and 64 GiB storage, and pass through no GPU devices. Set the LXC memory-lock limit to 1 GiB
(`lxc.prlimit.memlock: 1073741824`) to match the application container. Install Docker,
Compose and Python 3 with venv support. Use a fresh Aimee Compose project and
private credentials as below; also set `AIMEE_LLM_VARIANT=e2b`,
`SYNTHESIS_MODEL=gemma-4-E2B-it` and
`SYNTHESIS_ENDPOINT=https://aimee-llm:8761` in its private environment file.

```sh
scripts/compose-local.sh --env-file ~/.config/aimee-discord/application.env \
  -f compose.yaml -f integrations/discord/compose.cpu.yaml --profile synthesis up -d
```

The override caps inference at ten CPU cores, selects ten generation threads,
sets GPU layers to zero and uses a 2048-token context. The standard model service
requires the dedicated Server's mTLS identity and remains on the private model
network, with its authenticated TLS port published only on LXC loopback at 19852.
For the bridge set `endpoint` to `https://127.0.0.1:19852/v1/chat/completions`,
`model` to the ID returned by the model's `/v1/models` route, and `model_tls_dir`
to the dedicated project's `aimee-model-tls` volume's `synthesis/client` directory
(use `docker volume inspect` to obtain the mountpoint). The bridge verifies the
model certificate against that CA with server name `aimee-llm` and presents its
client certificate. Keep the live volume path, rather than copying expiring identities.
CPU inference uses this mTLS identity instead of the vLLM API key.
The CPU model supplies inference; the next section connects Aimee retrieval. It does
not perform the native memory capture described for the GPU vLLM integration below.

## Connect CPU chat to the dedicated Aimee store

Register a tools-disabled model named `discord-e2b` in the dedicated Aimee instance,
with endpoint `https://aimee-llm:8761/v1`, the CPU model's exact served ID, a 2048-token
context and a 384-token output limit. Use the instance's model settings or its local
`POST /v1/model/add` route. Its typed `args` array is:

```json
["discord-e2b", "https://aimee-llm:8761/v1",
 "unsloth/gemma-4-E2B-it-qat-GGUF:qat-UD-Q4_K_XL",
 "--provider", "openai", "--auth-type", "none", "--context-window", "2048",
 "--max-parallel", "1", "--max-tokens", "384", "--max-output", "384", "--tools", "off"]
```

Wrap this array as `{"args": [...]}` when calling the route. Aimee authenticates
its CPU model calls using its existing synthesis client certificate.

Set the bridge's `model` to `discord-e2b`, remove `model_tls_dir`, and set
`aimee_socket` to the `aimee-server-home` volume's `aimee-http.sock`. For the default
project the socket is:
`/var/lib/docker/volumes/aimee-discord-bot_aimee-server-home/_data/aimee-http.sock`.
Use `docker volume inspect` to confirm this location. The HTTP endpoint can remain
`http://127.0.0.1:19852/v1/chat/completions`: the Unix connector selects the actual
transport and makes no TCP connection to this placeholder address.

For durable conversation memory, add a dedicated KB Compose project with fresh volumes
and the synthesis worker enabled. Do not connect a personal knowledge collection.
Use [compose.knowledge.cpu.yaml](compose.knowledge.cpu.yaml) with `compose.kb.yaml`;
the knowledge worker uses two CPU threads, zero GPU layers and an 8192-token extraction
context. The reply model remains the ten-thread, 2048-token CPU E2B instance above.

Build the bridge and updated memory module from this checkout:

```sh
docker build -f integrations/discord/Dockerfile -t aimee-discord-bridge:local .
docker build -f integrations/discord/Dockerfile.memory -t aimee-discord-memory:local .
```

In the private KB environment file, set `AIMEE_DISCORD_BRIDGE_IMAGE`,
`AIMEE_DISCORD_MEMORY_IMAGE`, `AIMEE_DISCORD_CONFIG_DIR` and
`AIMEE_DISCORD_SERVER_HOME_VOLUME` to these images, the private bridge configuration
directory and the dedicated chat server’s home volume. Use the standard Vault/Compose
bootstrap for the new project. For a 1.0.0 base, apply [height-ontology.sql](height-ontology.sql)
out of band through the deployment’s migration account before starting the bridge;
new source builds include this row in the generated schema. The migration preserves
existing operator definitions and relation IDs.

Use [config.knowledge.example.json](config.knowledge.example.json). The bridge runs as
UID/GID 1000 with all capabilities dropped, a read-only filesystem and read-only mounts.
Give its configuration directory mode 0700 and its five configuration/credential files
mode 0600, owned by UID/GID 1000. `knowledge.token` contains this dedicated KB’s bearer
credential. Only the bridge’s explicit files are mounted; its PostgreSQL credentials
remain outside the bridge. Stop the host bridge service before starting the sidecar
so two gateway clients do not answer the same mention.

The bridge shares the KB’s network namespace and uses authenticated loopback on port
8741. A published Docker port is a remote peer to the KB and does not establish user
write authority. Keep this distinction: do not weaken the KB’s authority checks.
The read-only chat server home mount supplies the Aimee Unix socket; model inference
still uses Aimee’s existing mTLS synthesis identity.

```sh
scripts/compose-local.sh --env-file ~/.config/aimee-discord/knowledge.env \
  -f compose.kb.yaml -f integrations/discord/compose.knowledge.cpu.yaml \
  --profile synthesis up -d
```

Each admitted turn follows this path:

1. Retrieve current typed assertions with up to four bounded keyword queries to `memory.search_assertions`,
   scoped to the fixed `discord:<guild>:<channel>` project. Deduplicate and fit complete
   assertions into the context budget, then generate an admission reply.
2. Archive the human statement and generated admission reply in the dedicated Aimee
   user store. An explicit rejection such as “That information is incorrect” withholds
   the statement from fact capture; it still remains in the archive. This is a bounded
   rejection detector, not a complete truth verifier.
3. Submit admitted human text to `memory.store` with stable Discord event idempotency
   and source metadata. Aimee captures the authenticated connector’s authority and
   queues its existing grounded fact compiler. Model-generated inferences retain
   model authority and normal review/promotion rules. Bot answers are never used as
   independent evidence of their own claims.
4. Exact named height statements commit synchronously through the normal ontology,
   entity identity, evidence, contradiction and audit gates. Full qualified names stay
   distinct; `has_height` is functional, so a correction supersedes the same subject’s
   prior height. Worker replay deduplicates the original source evidence.
5. Generate the final answer after capture, archive that generated response and deliver
   it. Append the recent shared chat cache only after successful Discord delivery.

Both archives and typed facts survive process restart. Every human in the public
channel uses the same fact scope; threads have separate scopes. Retrieval excludes
historical, candidate and superseded assertions. Authentication, capture or retrieval
failure stops delivery and logs only failure metadata. Lexical typed retrieval remains
available when a vector generation is unavailable; that degraded mode does not claim
vector qualification. Complete facts exceeding the context allowance are omitted.

Configurations without a knowledge endpoint retain the earlier read-only
`store=user` memory search path. They do not enable conversation fact capture.
With Aimee retrieval enabled, input is capped at at most 600 UTF-8 bytes, reduced
for system instructions; 384 bytes are reserved for the memory block. The CPU
override disables hidden thinking so the reply budget produces visible text.

## Prepare a separate Aimee environment and E2B

Create a dedicated Aimee Server instance using the standard [installation](../../docs/QUICKSTART.md)
and [composition](../../compose.yaml). Use a separate composition project and volumes rather than
the personal instance. Aimee 1.0.0 has the required `POST /v1/native/primitive` route; 0.4.6 does not.
The prepared [application environment](application.env.example) selects the published 1.0.0
images, a separate Compose project and loopback-only ports 18443/18743. Copy it into private
configuration, generate the three database credentials as in Quickstart, and start from the
repository root on a host with Docker:

```sh
umask 077
cp integrations/discord/application.env.example ~/.config/aimee-discord/application.env
for role in ADMIN MIGRATOR RUNTIME; do
  printf 'AIMEE_STORE_%s_PASSWORD=%s\n' "$role" "$(openssl rand -hex 32)" \
    >> ~/.config/aimee-discord/application.env
done
scripts/compose-local.sh --env-file ~/.config/aimee-discord/application.env \
  -f compose.yaml up -d
```

Run credential generation once for a new environment; retain the private file for that instance.
Create a dedicated bot account and enroll a separate thin-client profile as described in
[Thin client](../../docs/THIN_CLIENT.md). Do not connect the Discord model to a personal or
administrator enrollment: every native-memory record accessible to this identity can influence
a public channel reply. Keep only approved channel knowledge under the bot identity.

Install the signed Gemma4 E2B bundle prepared in [PR #3017](https://github.com/RakuenSoftware/aimee/pull/3017),
following its pinned-key verification instructions. The bundle contains adapter 0.3.3, shared-runtime
build 5 and the GGUF loader. Use a **separate CPython 3.12 model environment**, Linux x86-64,
glibc 2.39+ and the correct GPU-specific vLLM 0.30.0 runtime. The recorded E2B smoke used one
RX 7900 XTX with the `+rocm723` vendor runtime, ROCm 7.2, Torch 2.12.0 and Triton 3.7.1.
See [qualification](../../docs/releases/native-memory-v0.3.3/qualification/README.md) for its limits.
Do not install GPU packages into the Discord bridge environment.

Provide the existing Gemma4 E2B model/config/tokenizer directory and its UD-Q4_K_XL GGUF weights.
The preparation command checks the E2B configuration binding and GGUF magic, hashes the checkpoint,
and writes a new private configuration and model API key. It does not download weights, enroll
an identity, start inference or overwrite an existing serving configuration.

```sh
~/.local/share/aimee-discord/venv/bin/python ~/.local/share/aimee-discord/prepare_e2b.py \
  --model /absolute/path/to/gemma4-e2b-model \
  --gguf /absolute/path/to/gemma-4-E2B-it-UD-Q4_K_XL.gguf \
  --output ~/.config/aimee-discord \
  --model-key ~/.config/aimee-discord/model.key

~/.local/share/aimee-discord/model-venv/bin/aimee-gemma4-e2b \
  --root ~/.local/share/aimee-discord/e2b connect-aimee \
  --aimee-home /absolute/path/to/dedicated-bot-enrollment
```

Set `config.json`'s `model` to the exact `model` path in `serving.json`; vLLM uses this as the default
served model name. Keep `system_context` and `native_system_context` identical. The default instruction
is shared by both preparation and chat. Serving binds to loopback port 19852 with a generated API
key, synchronous eager V1, prefix caching disabled, one GPU and zero CPU weight offload.
The memory recipient and catalog are derived by the installed plugin from the enrolled identity.
They are never supplied by Discord message authors.

## Start after configuring credentials and the inference host

```sh
mkdir -p ~/.config/systemd/user
cp integrations/discord/aimee-discord{,-e2b}.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user start aimee-discord-e2b.service
# After E2B is ready and the bot token/config are present:
systemctl --user start aimee-discord.service
```

Mention the bot in the configured channel. Everyone in the same channel shares one conversation history. Each thread has its own
shared history, separate from the parent channel and other threads; servers stay separate. Four recent turn pairs are retained in memory for up to 24 hours, with at most 128
conversations; restart clears this recent cache. Durable capture remains in Aimee when the
knowledge endpoint is configured. The native-memory account remains the shared bot identity,
not each Discord user's personal Aimee identity. Requests are serialized with an eight-turn queue.
For the bounded 2048-token E2B configuration, prompts use a conservative UTF-8 byte budget
(at most 1000 bytes, reduced to account for system instructions) and trim older turn pairs to fit;
replies request at most 384 tokens. Queue overflow and failed inference
are logged as metadata, without message content. The bridge does not receive attachments or audio.

## Verification

```sh
python3 -m venv .venv-discord
.venv-discord/bin/python -m pip install -r integrations/discord/requirements.txt
.venv-discord/bin/python -m unittest discover -s integrations/discord -v
```

Tests exercise the real HTTP model wire against a local fake endpoint, key rotation, redirect refusal,
native admission failure, durable capture, rejected-claim archival, capture failure, same-turn
post-commit replies, typed recall, channel admission, shared channel history across users, thread/server isolation, webhook payloads, mention suppression, Unicode
splitting, loop prevention, queue bounds and history retention. Actual E2B inference and Discord
conversation validation require the configured GPU host, bot application token and dedicated
Aimee enrollment. A webhook-only check does not establish end-to-end chatbot readiness.
