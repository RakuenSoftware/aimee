"""Print the embedding identity of an embedder image without serving it.

Fed on stdin to the embedder image's own interpreter by the Compose startup
helpers, before any service exists:

    docker compose run --rm --no-deps -T --entrypoint /opt/aimee/embedder-venv/bin/python \
        aimee-embedder - < scripts/embedder-identity.py

It loads the image's serving code and model exactly as the service would, then
prints one JSON line with the same identity fields the service reports on
/health. Memory provisioning pins that identity; nothing is sent anywhere.
"""
import importlib.util
import json
import os
import sys

SERVER = '/opt/aimee/scripts/embedder-server.py'


def main():
    # The image's entrypoint selects its baked model this way; this run bypasses it.
    if not os.environ.get('EMBEDDER_MODEL') and os.environ.get('AIMEE_EMBEDDER'):
        os.environ['EMBEDDER_MODEL'] = os.environ['AIMEE_EMBEDDER']
    spec = importlib.util.spec_from_file_location('aimee_embedder_server', SERVER)
    server = importlib.util.module_from_spec(spec)
    stdout, sys.stdout = sys.stdout, sys.stderr  # keep the model loader's chatter off the result
    try:
        spec.loader.exec_module(server)
        server._background_load()
        identity = server.embedding_identity()
    finally:
        sys.stdout = stdout
    if identity is None:
        print('embedder-identity: this embedder reports no complete identity', file=sys.stderr)
        return 1
    print(json.dumps({'status': 'ok', 'dim': identity['dimensions'], 'embedding_identity': identity,
                      'embedding_identity_digest': server.embedding_identity_digest(identity)}, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
