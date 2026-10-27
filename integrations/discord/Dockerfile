# Build the shared thinclient transport and its canonical chat collector.
FROM python:3.13-slim-bookworm AS thin-client
RUN apt-get update && apt-get install -y --no-install-recommends cmake make gcc libc6-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY . .
RUN cmake -S . -B /build -DAIMEE_THIN_CLIENT=ON -DAIMEE_LEAN=ON -DCMAKE_BUILD_TYPE=MinSizeRel && cmake --build /build --target aimee-client -j2

FROM python:3.13-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=thin-client /build/libaimee-client.so /usr/local/lib/libaimee-client.so
WORKDIR /app
COPY integrations/discord/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt
COPY integrations/discord/bridge.py integrations/discord/behavior.py integrations/discord/workflow_chat.py integrations/discord/conversation_memory.py integrations/discord/conversation_projection.py integrations/discord/model_ir.py integrations/discord/model_client.py ./
ENTRYPOINT ["python", "bridge.py"]
