#!/usr/bin/env python3
"""Enforce the unified application, optional KB and encrypted store packaging."""
from __future__ import annotations
import argparse
import copy
import posixpath
import re
import sys
from pathlib import Path
import yaml

REQUIRED_DOCKERIGNORE_ENTRIES = {
    ".git",
    ".aimee",
    ".env",
    "build",
    "src/build",
    "frontend/node_modules",
    # Exclude stale host binaries from the build context. This does not hide the
    # multi-stage builder's /src/aimee-kb output, which is created inside the image.
    "/aimee",
    "/aimee-server",
    "/aimee-kb",
    "/aimee-runtime-web",
    "/aimee-gateway",
}

FORBIDDEN_DOCKERIGNORE_ENTRIES = {
    "Dockerfile",
    "compose.yaml",
    "src",
    "src/**",
    "scripts",
    "scripts/**",
}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def normalized_dockerignore(path: Path) -> set[str]:
    entries: set[str] = set()
    for raw in read(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        negated = line.startswith("!")
        value = line[1:] if negated else line
        rooted = value.startswith("/")
        value = posixpath.normpath(value.rstrip("/"))
        if value == ".":
            continue
        if rooted and not value.startswith("/"):
            value = "/" + value
        entries.add(("!" if negated else "") + value)
    return entries


def missing_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if not re.search(pattern, text)]


def present_patterns(text: str, patterns: dict[str, str]) -> list[str]:
    return [name for name, pattern in patterns.items() if re.search(pattern, text)]


def has_compose_interpolation(value: object) -> bool:
    """Return true for Compose $VAR/${VAR}; $$ is a literal dollar escape."""
    text = str(value)
    index = 0
    while index < len(text):
        if text[index] != "$":
            index += 1
            continue
        if index + 1 < len(text) and text[index + 1] == "$":
            index += 2
            continue
        if index + 1 < len(text) and (text[index + 1] == "{" or re.match(r"[A-Za-z_]", text[index + 1])):
            return True
        index += 1
    return False


if yaml is not None:
    class UniqueKeySafeLoader(yaml.SafeLoader):
        """SafeLoader that rejects duplicate lexical mapping keys before merge resolution."""


    def _construct_unique_mapping(loader: UniqueKeySafeLoader, node: yaml.MappingNode, deep: bool = False):
        seen: set[tuple[str, str]] = set()
        for key_node, _ in node.value:
            if str(key_node.value) == "<<" or key_node.tag == "tag:yaml.org,2002:merge":
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    "YAML merge keys are unsupported by this fail-closed packaging gate",
                    key_node.start_mark,
                )
            identity = (key_node.tag, str(key_node.value))
            if identity in seen:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    f"duplicate mapping key {key_node.value!r}",
                    key_node.start_mark,
                )
            seen.add(identity)
        return yaml.SafeLoader.construct_mapping(loader, node, deep=deep)


    UniqueKeySafeLoader.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_unique_mapping
    )

# Compose override lists have ordinary list contents. Their replacement semantics
# are checked below against the one approved parent, not silently flattened.
UniqueKeySafeLoader.add_constructor('!override', lambda loader, node: _construct_unique_mapping(loader, node, deep=True) if isinstance(node, yaml.MappingNode) else loader.construct_sequence(node, deep=True))


def parse(text: str) -> dict:
    model = yaml.load(text, Loader=UniqueKeySafeLoader)
    if not isinstance(model, dict):
        raise ValueError('Compose root must be a mapping')
    return model


def port_failures(ports: object) -> list[str]:
    failures = []
    if not isinstance(ports, list) or len(ports) != 2:
        return ['KB must publish exactly its loopback health port and authenticated TLS port']
    seen = set()
    for entry in ports:
        if not isinstance(entry, str):
            failures.append('KB ports require explicit quoted host/published/target strings')
            continue
        # Only the published number is configurable. The interface, target and
        # protocol stay fixed, so operator substitution cannot expose HTTP.
        normalized = re.sub(r'\$\{AIMEE_KB_(?:HEALTH_)?PORT:-([0-9]+)\}', r'\1', entry)
        match = re.fullmatch(r'(?:(127\.0\.0\.1|\[::1\]):)?([0-9]+):(8741|8745)(?:/tcp)?', normalized)
        if not match or not 1 <= int(match[2]) <= 65535:
            failures.append('unsupported KB port publication: ' + entry)
            continue
        host, _, target = match.groups()
        if target == '8741' and host not in ('127.0.0.1', '[::1]'):
            failures.append('KB plaintext health port must remain loopback-only')
        if target in seen:
            failures.append('duplicate KB port publication')
        seen.add(target)
    if seen != {'8741', '8745'}:
        failures.append('KB needs distinct health and TLS publications')
    return failures


def composition_failures(base: dict, kb: dict, managed: dict) -> list[str]:
    failures = []
    services = base.get('services', {})
    if set(services) != {'aimee-server', 'aimee-store-db', 'aimee-embedder', 'aimee-llm'}:
        failures.append('standard composition must install only Server, PostgreSQL and model services')
        return failures
    app, pg = services['aimee-server'], services['aimee-store-db']
    env = app.get('environment', {})
    if env.get('AIMEE_INSTANCE_ROLE') != 'server':
        failures.append('base composition must establish Server identity')
    if app.get('build', {}).get('dockerfile') != 'Dockerfile.server':
        failures.append('application must use the unified Dockerfile')
    if pg.get('build', {}).get('dockerfile') != 'Dockerfile.postgres':
        failures.append('PostgreSQL must use the standardized encrypted image')
    if pg.get('ports') or pg.get('privileged') or pg.get('network_mode'):
        failures.append('PostgreSQL must stay on its private network with explicit device capabilities')
    if pg.get('networks') != ['store'] or base.get('networks', {}).get('store', {}).get('internal') is not True:
        failures.append('PostgreSQL network must be internal')
    if app.get('depends_on', {}).get('aimee-store-db', {}).get('condition') != 'service_started':
        failures.append('application must unlock PostgreSQL before waiting for SQL health')
    if env.get('AIMEE_POSTGRES_STORAGE_SOCKET') != '/run/aimee-postgres/storage.sock':
        failures.append('application must attach the PostgreSQL unlock resource')
    for name in ('AIMEE_STORE_URL', 'AIMEE_STORE_MIGRATION_URL'):
        if name in env:
            failures.append(name + ' must be supplied through the owning Vault, not Config.Env')
    for required in ('aimee-server-home:/var/lib/aimee', 'aimee-store-tls:/run/aimee-store-tls:ro', 'aimee-postgres-control:/run/aimee-postgres:ro'):
        if required not in app.get('volumes', []):
            failures.append('application missing required private resource mount ' + required)
    if 'aimee-postgres-encrypted:/var/lib/aimee-postgres' not in pg.get('volumes', []):
        failures.append('PostgreSQL must persist its encrypted image')
    if any('/var/lib/postgresql/data' in str(v) for v in pg.get('volumes', [])):
        failures.append('plaintext PostgreSQL data must never be a persistent volume')
    for name in ('aimee-model-tls', 'aimee-embedding-tls', 'aimee-synthesis-tls'):
        options = base.get('volumes', {}).get(name, {}).get('driver_opts', {})
        if options.get('type') != 'tmpfs' or 'mode=0700' not in str(options.get('o', '')):
            failures.append(name + ' must materialize private identities only in tmpfs')
    for service, profile in (('aimee-embedder', 'embedding'), ('aimee-llm', 'synthesis')):
        model = services[service]
        expected = f'aimee-{profile}-tls:/run/aimee-model-tls/{profile}/server:ro'
        if model.get('user') != '1000:1000' or model.get('volumes') != [expected]:
            failures.append(service + ' must receive only its server identity, read-only as uid 1000')
        if model.get('ports') or model.get('network_mode') or model.get('privileged'):
            failures.append(service + ' must remain on the private model network')
    if services['aimee-llm'].get('profiles') != ['synthesis']:
        failures.append('synthesis must remain optional')
    kb_services = kb.get('services', {})
    if set(kb_services) != {'aimee-kb', 'aimee-store-db', 'aimee-embedder', 'aimee-llm'}:
        failures.append('KB must use the same PostgreSQL and model composition')
    role = kb_services.get('aimee-kb', {})
    if role.get('extends') != {'file': 'compose.yaml', 'service': 'aimee-server'}:
        failures.append('KB must inherit the unified application')
    if kb_services.get('aimee-store-db', {}).get('extends') != {'file': 'compose.yaml', 'service': 'aimee-store-db'}:
        failures.append('KB must inherit standardized PostgreSQL')
    if any(name in role for name in ('image', 'entrypoint', 'command', 'volumes', 'network_mode', 'privileged')):
        failures.append('KB must not override the shared image, startup or custody mounts')
    kb_env = role.get('environment', {})
    if kb_env.get('AIMEE_INSTANCE_ROLE') != 'kb' or kb_env.get('AIMEE_KB_MTLS_PORT') != '8745':
        failures.append('KB must establish KB identity and enable authenticated TLS')
    if not str(kb.get('x-aimee-vault', {}).get('AIMEE_KB_API_BEARER_TOKEN', '')).startswith('${AIMEE_KB_API_BEARER_TOKEN:?'):
        failures.append('KB requires an explicit first-boot authority credential')
    if any(name in kb_env for name in ('AIMEE_DB2_URL', 'AIMEE_KB_API_BEARER_TOKEN')):
        failures.append('KB database and authority credentials must enter Vault through stdin')
    if '/v1/health' not in str(role.get('healthcheck', '')):
        failures.append('KB must probe its actual health route')
    failures.extend(port_failures(role.get('ports')))
    if set(managed.get('services', {})) != {'aimee-embedder', 'aimee-llm'}:
        failures.append('wizard may install only model services')
    for service in managed.get('services', {}).values():
        if any('/var/lib/aimee' in str(v) or '/client' in str(v) for v in service.get('volumes', [])):
            failures.append('managed model must not receive the instance Vault or client identities')
    return failures


def check(root: Path) -> list[str]:
    failures = []
    try:
        base, kb, managed = [parse(read(root / p)) for p in ('compose.yaml', 'compose.kb.yaml', 'deploy/container/aimee-managed.compose.yaml')]
        failures.extend(composition_failures(base, kb, managed))
        for path in sorted(root.glob('compose*.yaml')):
            model = parse(read(path))
            for name, service in model.get('services', {}).items():
                if not isinstance(service, dict) or any(v is None for v in service.values()):
                    failures.append(path.name + ': empty service configuration ' + name)
        app = read(root / 'Dockerfile.server')
        if read(root / 'Dockerfile') != app:
            failures.append('default Dockerfile must build the same unified application')
        for binary in ('aimee-server', 'aimee-kb', 'aimee-kb-worm'):
            if not re.search(r'COPY\s+--from=build\s+/src/' + binary + r'\s+/usr/local/bin/' + binary, app):
                failures.append('unified image missing ' + binary)
        pg = read(root / 'Dockerfile.postgres')
        for requirement in ('cryptsetup-bin', 'postgresql-18-pgvector', 'postgres-secure-entrypoint.sh', 'aimee-postgres-storage'):
            if requirement not in pg:
                failures.append('standard PostgreSQL image missing ' + requirement)
        if 'postgresql-${PG_MAJOR}' in app or re.search(r'(?m)^USER\s+postgres', app):
            failures.append('application must not contain its own PostgreSQL engine')
        entries = normalized_dockerignore(root / '.dockerignore')
        failures += ['.dockerignore missing ' + e for e in sorted(REQUIRED_DOCKERIGNORE_ENTRIES - entries)]
        hidden = {e.lstrip('/') for e in entries if not e.startswith('!')}
        failures += ['.dockerignore hides required build input ' + e for e in sorted(FORBIDDEN_DOCKERIGNORE_ENTRIES & hidden)]
        defaults = parse(read(root / 'deploy/container/aimee-server-remote-writes.yaml'))
        if defaults.get('aimee', {}).get('api', {}).get('remote_writes') != 'off':
            failures.append('Server default remote_writes must remain off')
        tls = read(root / 'src/server/server_tls.c')
        if not all(marker in tls for marker in ('"%s/tls/server.crt"', 'config_default_dir()', 'pki_ensure_self_signed_server_cert')):
            failures.append('Server TLS identity must remain rooted in its own home')
    except (OSError, ValueError, yaml.YAMLError, TypeError, KeyError) as exc:
        failures.append('invalid packaging input: ' + str(exc))
    return failures


def plant_test(root: Path) -> int:
    base, kb, managed = [parse(read(root / p)) for p in ('compose.yaml', 'compose.kb.yaml', 'deploy/container/aimee-managed.compose.yaml')]
    plants = [
        ('base installs KB', lambda b,k,m: b['services'].update({'aimee-kb': {}})),
        ('plaintext PG volume', lambda b,k,m: b['services']['aimee-store-db']['volumes'].append('plaintext:/var/lib/postgresql/data')),
        ('wrong role', lambda b,k,m: k['services']['aimee-kb']['environment'].update(AIMEE_INSTANCE_ROLE='server')),
        ('PG health deadlock', lambda b,k,m: b['services']['aimee-server']['depends_on']['aimee-store-db'].update(condition='service_healthy')),
        ('disabled database TLS', lambda b,k,m: b['services']['aimee-server']['environment'].update(AIMEE_STORE_URL='host=pg password=leaked sslmode=disable')),
        ('persistent model keys', lambda b,k,m: b['volumes']['aimee-model-tls'].update(driver_opts={'type':'none'})),
        ('model obtains client key', lambda b,k,m: b['services']['aimee-llm']['volumes'].append('client:/run/client')),
        ('wizard installs KB', lambda b,k,m: m['services'].update({'aimee-kb': {}})),
        ('KB bypasses unified image', lambda b,k,m: k['services']['aimee-kb'].update(image='other')),
    ]
    for name, mutate in plants:
        b,k,m = copy.deepcopy((base,kb,managed)); mutate(b,k,m)
        if not composition_failures(b,k,m):
            raise AssertionError('missed mutation: ' + name)
    for port in ('8741:8741', '[::]:8741:8741', '0.0.0.0:8741:8741', '127.0.0.1:8700-8750:8700-8750', '${HOST}:8741:8741', 8741, {'target':8741,'published':8741}):
        if not port_failures([port, '8745:8745']):
            raise AssertionError('missed unsafe port: ' + str(port))
    for good in ('127.0.0.1:18741:8741', '[::1]:8741:8741', '127.0.0.1:${AIMEE_KB_HEALTH_PORT:-8741}:8741'):
        if port_failures([good, '${AIMEE_KB_PORT:-8745}:8745']):
            raise AssertionError('rejected valid loopback port: ' + good)
    for invalid in ('services:\n  x: {}\n  x: {}\n', 'services:\n  x: {<<: {image: malicious}}\n'):
        try:
            parse(invalid)
        except yaml.YAMLError:
            pass
        else:
            raise AssertionError('ambiguous YAML accepted')
    print('container-packaging: mutation tests passed')
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--plant-test', action='store_true')
    args = parser.parse_args()
    if args.plant_test:
        raise SystemExit(plant_test(args.root.resolve()))
    failures = check(args.root.resolve())
    for failure in failures:
        print('container-packaging: ' + failure, file=sys.stderr)
    if not failures:
        print('container-packaging: unified application and encrypted store checks passed')
    raise SystemExit(bool(failures))
