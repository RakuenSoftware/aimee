#!/usr/bin/env python3
"""Provision the memory installation of one Aimee instance. Idempotent.

Runs as root in the one-shot `aimee-memory-init` service, before the
application container exists. The image carries the memory bundle (executables
and provisioning scripts) under /opt/aimee/memory; this command turns it into an
installation that belongs to this deployment:

  fresh    no installation yet: generate the instance's memory identity, grants
           and owner key, prepare /opt/aims-memory/<role>, verify it, and create
           the writable subtree <home>/private-memory/<role>.
  current  the installation matches this image: verify and change nothing.
  update   the installation was prepared by a different image, or the
           requested accounts or embedding provider changed: prepare again
           under the same memory identity while keeping the owner key and the
           stored memory, then switch the configuration over. Refused while the
           application is running, except that a changed account list, shared
           scope or embedder keeps the current installation until the next start.

A KB serves the shared scopes in AIMEE_MEMORY_SHARED_SCOPES: `global` always, and
by default every project and workspace (`project:*`, `workspace:*`); listing
`project:NAME` / `workspace:NAME` instead limits it to those.

The embedding provider is pinned by identity. The startup helpers read it from
the embedder image and pass it on standard input; without input the recorded
provider is kept, and a first provisioning without one has no vector retrieval.

Nothing here starts a service. The application mounts /opt/aims-memory
read-only and only the three directories of the home subtree read-write.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

BUNDLE = Path(os.environ.get('AIMEE_MEMORY_BUNDLE', '/opt/aimee/memory'))
ROOT = Path('/opt/aims-memory')
HOME = Path(os.environ.get('AIMEE_HOME', '/var/lib/aimee'))
RUNTIME_USER = 'aimee'
NAMESPACE_PREFIX = 'aimee'
DB_REF, OUTBOUND_REF, SCHEMA_ADMIN_REF = 180, 181, 182
MUTABLE = ('etc', 'state', 'run')
BUNDLE_COPIES = {'libexec/aims-db-module': 'aims-db-module', 'libexec/aimee-memory-module': 'aimee-memory-module',
                 'libexec/memory-profile': 'memory-profile',
                 'libexec/aims-overlay-schema-bootstrap': 'aims-overlay-schema-bootstrap'}


class Refused(Exception):
    """A condition the operator has to resolve; the message says how."""


def log(message: str) -> None:
    print(f'[memory-provision] {message}', flush=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def accounts(role: str, value: str | None) -> list[str]:
    """The accounts whose memory this installation serves.

    A Server serves each named account's personal memory; a KB has one shared
    owner account. Grants name accounts exactly, so the list is re-evaluated at
    every start and an added or removed account re-prepares the installation.
    """
    names = [name.strip() for name in (value or RUNTIME_USER).split(',') if name.strip()]
    if role == 'kb':
        return [RUNTIME_USER]
    if RUNTIME_USER not in names:
        names.insert(0, RUNTIME_USER)
    if len(names) > 32 or len(set(names)) != len(names):
        raise Refused('AIMEE_MEMORY_ACCOUNTS must name at most 32 distinct accounts')
    for name in names:
        if len(name) > 128 or any(ord(char) < 33 or ord(char) > 126 for char in name):
            raise Refused(f'AIMEE_MEMORY_ACCOUNTS has an invalid account name: {name!r}')
    return names


def browser_logins(home: Path) -> list[str]:
    """Browser accounts that can sign in, from the record runtime-web keeps in the home volume.

    Each line is `<user>:<shadow verifier>`; a locked or empty verifier is not a
    usable login. Only names are read. The first-boot login is included while
    it has not been replaced.
    """
    names = []
    record = home / 'webchat' / 'identities'
    if record.is_file():
        for line in record.read_text(errors='replace').splitlines():
            name, _, verifier = line.partition(':')
            if name and verifier and verifier[0] not in '!*':
                names.append(name)
    bootstrap, replaced = home / 'webchat' / 'bootstrap-user', home / 'webchat' / 'bootstrap-replaced'
    if bootstrap.is_file() and not replaced.exists():
        # One line, `<origin>:<name>` (origin `generated` or `explicit`).
        names.append(bootstrap.read_text(errors='replace').strip().partition(':')[2])
    valid = [name for name in names if name and len(name) <= 128 and all(32 < ord(char) < 127 for char in name)]
    return sorted(set(valid))


def served_accounts(role: str, value: str | None, home: Path) -> str:
    """The configured account list plus every browser login, as one list for `accounts`."""
    names = [name.strip() for name in (value or RUNTIME_USER).split(',') if name.strip()]
    if role == 'server':
        for name in browser_logins(home):
            if name not in names:
                names.append(name)
        if RUNTIME_USER not in names:
            names.insert(0, RUNTIME_USER)
        if len(names) > 32:
            log(f'{len(names)} accounts would be served but memory serves at most 32; '
                f'not served: {", ".join(names[32:])}. Name the ones to serve first in AIMEE_MEMORY_ACCOUNTS')
            names = names[:32]
    return ','.join(names)


OPEN_SHARED_SCOPES = 'global,project:*,workspace:*'


def shared_scopes(role: str, value: str | None) -> list[str]:
    """The shared scopes a KB serves.

    `global` is always served. `project:*` and `workspace:*` serve every project
    or workspace a caller names (the default); `project:NAME` and
    `workspace:NAME` serve one each, for a KB limited to a fixed list.
    """
    if role != 'kb':
        return []
    names = [name.strip() for name in (value or OPEN_SHARED_SCOPES).split(',') if name.strip()]
    if 'global' not in names:
        names.insert(0, 'global')
    if len(names) > 96 or len(set(names)) != len(names):
        raise Refused('AIMEE_MEMORY_SHARED_SCOPES must name at most 96 distinct scopes')
    for name in names:
        kind, _, scope = name.partition(':')
        if name in ('project:*', 'workspace:*'):
            continue
        if name != 'global' and (kind not in ('project', 'workspace') or not scope or len(scope) > 128
                                 or scope.startswith('_') or any(ord(char) < 33 or ord(char) > 126 for char in scope)):
            raise Refused(f'AIMEE_MEMORY_SHARED_SCOPES has an invalid scope: {name!r} '
                          '(use global, project:*, workspace:*, project:NAME or workspace:NAME)')
    return names


def stale_reasons(prefix: Path, role: str, bundle: Path = BUNDLE) -> list[str]:
    """Why a prepared installation does not belong to this image; empty when it does."""
    manifest = json.loads((prefix / 'overlay-manifest.json').read_text())
    files = manifest.get('files', {})
    reasons = []
    for relative, name in BUNDLE_COPIES.items():
        if files.get(relative) != sha256(bundle / 'bin' / name):
            reasons.append(f'memory executable {name} differs')
    resource = Path(f'/usr/local/bin/aimee-{role}')
    if not resource.is_file() or files.get(f'bin/aimee-{role}') != sha256(resource):
        reasons.append(f'aimee-{role} differs')
    for original, pin in (manifest.get('container_public_executables') or {}).items():
        path = Path(original)
        if not path.is_file() or sha256(path) != pin.get('sha256'):
            reasons.append(f'{original} differs')
    return reasons


def application_running(role: str, home: Path = HOME) -> bool:
    """True when something accepts connections on this role's module bus socket."""
    path = home / f'{role}-module-bus.sock'
    if not path.exists():
        return False
    for kind in (socket.SOCK_STREAM, socket.SOCK_SEQPACKET):
        probe = socket.socket(socket.AF_UNIX, kind)
        probe.settimeout(2)
        try:
            probe.connect(str(path))
            return True
        except OSError:
            continue
        finally:
            probe.close()
    return False


def as_runtime_user(*argv: str) -> None:
    command = ['runuser', '-u', RUNTIME_USER, '--', 'env', 'PYTHONDONTWRITEBYTECODE=1', 'python3', *argv]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in result.stdout.splitlines():
        log(line)
    if result.returncode:
        raise Refused(f'{Path(argv[0]).name} failed with exit {result.returncode}')


def owned(path: Path) -> None:
    shutil.chown(path, RUNTIME_USER, RUNTIME_USER)


def prepare(role: str, prefix: Path, inputs: Path, adopt: Path | None) -> None:
    provision, binaries = BUNDLE / 'provision', BUNDLE / 'bin'
    scope = 'personal' if role == 'server' else 'shared'
    argv = [str(provision / 'prepare.py'), '--container-install', '--aimee-source', str(BUNDLE / 'source'),
            '--role', role, '--prefix', str(prefix),
            '--module-manifest', f'/opt/aimee/module-grants/{role}.modules',
            '--grant-dir', f'/opt/aimee/module-grants/{role}',
            # Plugin grants an operator installs in the instance's own module
            # directory join the installation's policy at each start.
            '--operator-grants', f'/var/lib/aimee/modules.d/{role}',
            '--db-config', str(inputs / 'database.json'), '--frontend-config', str(inputs / 'frontend.json'),
            '--resource-binary', f'/usr/local/bin/aimee-{role}',
            '--db-binary', str(binaries / 'aims-db-module'), '--memory-binary', str(binaries / 'aimee-memory-module'),
            '--profile-binary', str(binaries / 'memory-profile'),
            '--bootstrap-binary', str(binaries / 'aims-overlay-schema-bootstrap'),
            '--owner-bootstrap-binary', str(binaries / 'aims-db-owner-bootstrap'),
            '--db-ref', str(DB_REF), '--outbound-ref', str(OUTBOUND_REF),
            '--schema-admin', f'{NAMESPACE_PREFIX}-{scope}={SCHEMA_ADMIN_REF}']
    if adopt is not None:
        argv += ['--adopt-source-owner', str(adopt)]
    if (inputs / 'processing-plan.json').is_file():
        # Typed facts: the memory module's processing operator calls the
        # synthesis model on the KB's global scope or the Server account's
        # personal memory; aims-db stores the facts.
        argv += ['--processing-plan', str(inputs / 'processing-plan.json'),
                 '--processing-worker-binary', str(binaries / 'memory-processing-operator'),
                 '--typed-facts-synthesis', str(inputs / 'typed-facts-synthesis.json')]
    as_runtime_user(*argv)
    as_runtime_user(str(provision / 'verify.py'), str(prefix))


def install_home(prefix: Path) -> None:
    """Create or confirm the writable subtree; the prepared installer verifies every pin."""
    result = subprocess.run(['python3', str(prefix / 'libexec/compose_install_home.py'), '--prefix', str(prefix)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in result.stdout.splitlines():
        log(line)
    if result.returncode:
        raise Refused('the writable memory subtree could not be initialized or does not match the installation')


def supplied_identity(raw: bytes) -> dict | None:
    """The embedder health document handed over on standard input, if any."""
    if not raw.strip():
        return None
    try:
        health = json.loads(raw)
    except ValueError as error:
        raise Refused('standard input is not an embedder identity document') from error
    if not isinstance(health, dict) or not isinstance(health.get('embedding_identity'), dict) \
            or not isinstance(health.get('embedding_identity_digest'), str):
        raise Refused('standard input is not an embedder identity document')
    return health


def recorded_policy(inputs: Path) -> dict | None:
    """What the saved inputs were generated for, or None before first provisioning."""
    report = inputs / 'OPERATOR_POLICY_SAFE.json'
    if not report.is_file():
        if inputs.exists():
            raise Refused(f'{inputs} is incomplete; restore the home volume from backup')
        return None
    policy = json.loads(report.read_text())
    policy['typed_facts'] = recorded_typed_facts(inputs)
    return policy


def desired_policy(role: str, recorded: dict | None, requested_accounts: str | None, health: dict | None,
                   requested_scopes: str | None = None) -> dict:
    """Accounts, shared scopes and embedding provider this run asks for; unspecified parts keep the recorded value."""
    if requested_accounts is None and recorded is not None:
        names = list(recorded['accounts'])
    else:
        names = accounts(role, requested_accounts)
    if requested_scopes is None and recorded is not None:
        scopes = list(recorded.get('shared_scopes', []))
    else:
        scopes = shared_scopes(role, requested_scopes)
    if health is not None:
        digest = health['embedding_identity_digest']
    else:
        digest = recorded.get('embedding_provider_identity_digest') if recorded else None
    return {'accounts': names, 'shared_scopes': scopes, 'embedding_provider_identity_digest': digest,
            'typed_facts': typed_facts(role)}


# The client identity a managed KB issues itself for the aimee-llm sidecar.
MANAGED_SYNTHESIS_IDENTITY = {'ca_file': '/run/aimee-model-tls/synthesis/client/ca.pem',
                              'cert_file': '/run/aimee-model-tls/synthesis/client/client.pem',
                              'key_file': '/run/aimee-model-tls/synthesis/client/client.key'}


def typed_facts(role: str) -> dict | None:
    """The synthesis model an instance derives typed facts with, or None.

    The same settings the KB's synthesis client uses: SYNTHESIS_ENDPOINT and
    SYNTHESIS_MODEL, and for an https endpoint the client identity in
    SYNTHESIS_CA_FILE / SYNTHESIS_CERT_FILE / SYNTHESIS_KEY_FILE. With managed model
    services the KB's client always presents the identity it issued for the
    aimee-llm sidecar (agent_bridge.c), so that is the default here too. A Server
    processes its account's personal memory and a KB its shared memory;
    AIMEE_MEMORY_TYPED_FACTS=off keeps either plain."""
    if os.environ.get('AIMEE_MEMORY_TYPED_FACTS', 'on') == 'off':
        return None
    endpoint = (os.environ.get('SYNTHESIS_ENDPOINT') or '').rstrip('/')
    model = os.environ.get('SYNTHESIS_MODEL') or ''
    if not endpoint or not model:
        return None
    upstream = endpoint if endpoint.endswith('/v1') else endpoint + '/v1'
    selection = {'model': model, 'upstream': upstream}
    if upstream.startswith('https://'):
        files = {key: os.environ.get(env) or '' for key, env in
                 (('ca_file', 'SYNTHESIS_CA_FILE'), ('cert_file', 'SYNTHESIS_CERT_FILE'),
                  ('key_file', 'SYNTHESIS_KEY_FILE'))}
        if os.environ.get('AIMEE_MODEL_SERVICES_ENABLED') == '1':
            files = dict(MANAGED_SYNTHESIS_IDENTITY)
        if not all(files.values()):
            log('typed facts stay off: the https synthesis endpoint has no client identity '
                '(SYNTHESIS_CA_FILE, SYNTHESIS_CERT_FILE, SYNTHESIS_KEY_FILE)')
            return None
        selection.update(files)
    elif not upstream.startswith('http://127.0.0.1:'):
        log('typed facts stay off: a plain http synthesis endpoint must be loopback')
        return None
    return selection


def recorded_typed_facts(inputs: Path) -> dict | None:
    path = inputs / 'TYPED_FACTS.json'
    return json.loads(path.read_text()) if path.is_file() else None


def policy_changes(recorded: dict, desired: dict) -> list[str]:
    changes = []
    if list(recorded['accounts']) != desired['accounts']:
        changes.append('accounts')
    if list(recorded.get('shared_scopes', [])) != desired['shared_scopes']:
        changes.append('shared scopes')
    if recorded.get('embedding_provider_identity_digest') != desired['embedding_provider_identity_digest']:
        changes.append('embedding provider')
    if recorded.get('typed_facts') != desired.get('typed_facts'):
        changes.append('typed facts')
    return changes


def write_inputs(role: str, inputs: Path, desired: dict, health: dict | None, node: str | None, previous: Path | None = None) -> None:
    """Generate operator inputs into a directory that must not exist yet."""
    argv = [str(BUNDLE / 'provision/operator_inputs.py'), '--role', role, '--namespace-prefix', NAMESPACE_PREFIX,
            '--output', str(inputs)]
    for name in desired['accounts']:
        argv += ['--account', name]
    for scope in desired['shared_scopes']:
        argv += ['--shared-scope', scope]
    if node is not None:
        argv += ['--node', node]
    handed = None
    if desired['embedding_provider_identity_digest'] is not None:
        if health is None:
            raise Refused('the embedding provider identity is needed to regenerate the memory configuration; '
                          'start through scripts/compose-local.sh so it can be read from the embedder image')
        handed = inputs.parent / f'.embedding-health-{role}.json'
        handed.write_text(json.dumps(health))
        handed.chmod(0o600)
        owned(handed)
        argv += ['--embedding-health', str(handed)]
    try:
        as_runtime_user(*argv)
    finally:
        if handed is not None:
            handed.unlink(missing_ok=True)
    selection = desired.get('typed_facts')
    if selection is not None:
        argv = [str(BUNDLE / 'provision/processing_kb_inputs.py'), '--inputs', str(inputs),
                '--profile-binary', str(BUNDLE / 'bin/memory-profile'), '--model', selection['model'],
                '--upstream', selection['upstream'],
                # The instance's ontology authority and graph outlive any one
                # generation of inputs.
                '--ontology-key', str(inputs.parent / f'typed-facts-{role}.key'),
                '--graph-file', str(inputs.parent / f'typed-facts-{role}.graph')]
        if previous is not None:
            argv += ['--previous-inputs', str(previous)]
        for key in ('ca_file', 'cert_file', 'key_file'):
            if selection.get(key):
                argv += ['--' + key.replace('_', '-'), selection[key]]
        as_runtime_user(*argv)
        record = inputs / 'TYPED_FACTS.json'
        record.write_text(json.dumps(selection, indent=2) + '\n')
        record.chmod(0o600)
        owned(record)


def switch_configuration(prefix: Path, installed: Path) -> None:
    """Point the kept state at the re-prepared configuration; the old one stays until this succeeds."""
    stamp = time.strftime('%Y%m%dT%H%M%S', time.gmtime())
    fresh, retired = installed / 'etc.new', installed / f'etc.retired-{stamp}'
    if fresh.exists():
        shutil.rmtree(fresh)
    shutil.copytree(prefix / 'etc', fresh, symlinks=False)
    for path in [fresh, *fresh.rglob('*')]:
        if path.is_dir():
            path.chmod(0o700)
        owned(path)
    identity = {'schema_version': 1, 'role': json.loads((prefix / 'overlay-manifest.json').read_text())['role'],
                'manifest_sha256': sha256(prefix / 'overlay-manifest.json')}
    marker = installed / 'installation.json'
    pending = installed / 'installation.json.new'
    pending.write_text(json.dumps(identity, indent=2) + '\n')
    pending.chmod(0o600)
    owned(pending)
    (installed / 'etc').rename(retired)
    fresh.rename(installed / 'etc')
    pending.rename(marker)
    shutil.rmtree(retired)


def placeholder(installed: Path, prefix: Path) -> None:
    """Mount points only: the application's volume mounts need both ends to exist.

    The application mounts three home subdirectories onto three directories of
    the read-only memory volume. With no installation, empty ones stand in.
    """
    if (installed / 'installation.json').is_file():
        log('provisioning is off; the existing memory installation is left as it is and stays active')
        return
    for base in (installed, prefix):
        for name in MUTABLE:
            path = base / name
            path.mkdir(mode=0o700, parents=True, exist_ok=True)
            owned(path)
        owned(base)
        base.chmod(0o700)


def remove_placeholder(installed: Path) -> None:
    """Drop empty mount points left by a run with provisioning off; never anything holding files."""
    if not installed.exists():
        return
    if installed.is_symlink() or any(path.is_file() or path.is_symlink() for path in installed.rglob('*')):
        raise Refused(f'{installed} holds files but no installation record; restore the home volume from backup '
                      'or move that directory away deliberately')
    shutil.rmtree(installed)


def provision(role: str, health: dict | None = None) -> str:
    prefix, installed = ROOT / role, HOME / 'private-memory' / role
    inputs = HOME / 'private-memory' / f'inputs-{role}'
    if not HOME.is_dir() or not ROOT.is_dir():
        raise Refused(f'{HOME} and {ROOT} must be mounted volumes')
    parent = HOME / 'private-memory'
    parent.mkdir(mode=0o700, exist_ok=True)
    owned(parent)
    shutil.chown(ROOT, RUNTIME_USER, RUNTIME_USER)

    if os.environ.get('AIMEE_MEMORY_PROVISION', 'on') == 'off':
        placeholder(installed, prefix)
        return 'off'
    if not (BUNDLE / 'provision/prepare.py').is_file():
        raise Refused('this image carries no memory bundle for its platform, so memory cannot be provisioned; '
                      'set AIMEE_MEMORY_PROVISION=off to run this instance without memory')
    recorded = recorded_policy(inputs)
    desired = desired_policy(role, recorded, served_accounts(role, os.environ.get('AIMEE_MEMORY_ACCOUNTS'), HOME),
                             health, os.environ.get('AIMEE_MEMORY_SHARED_SCOPES') or None)
    if not (installed / 'installation.json').is_file():
        # Nothing is stored yet, so the memory identity may be created or recreated freely.
        remove_placeholder(installed)
        for stale in (prefix, inputs):
            if stale.exists():
                shutil.rmtree(stale)
        write_inputs(role, inputs, desired, health, None)
        log(f'memory identity created for {role}; accounts: {", ".join(desired["accounts"])}; '
            + (f'shared scopes: {", ".join(desired["shared_scopes"])}; ' if desired['shared_scopes'] else '')
            + 'vector retrieval: '
            + ('on' if desired['embedding_provider_identity_digest'] else 'off (no embedder identity supplied)'))
        prepare(role, prefix, inputs, None)
        install_home(prefix)
        return 'fresh'
    if recorded is None:
        raise Refused(f'{inputs} is missing, so the installation cannot be verified or re-prepared; '
                      'restore the home volume from backup')

    prepared = (prefix / 'overlay-manifest.json').is_file()
    reasons = stale_reasons(prefix, role) if prepared else ['no prepared installation in the memory volume']
    reasons += [f'{change} changed' for change in policy_changes(recorded, desired)]
    if not reasons:
        install_home(prefix)
        return 'current'

    # Stored memory exists and the installation has to change. Keep the node,
    # the owner and the data; regenerate everything derived from them.
    running = application_running(role)
    if running and prepared and not stale_reasons(prefix, role):
        # Only the requested policy differs (for example a browser login created
        # while the stack ran). The installation is still valid for the running
        # application, so a repeated `up` succeeds and the change waits for the
        # next start instead of failing the command.
        log('memory installation is current; ' + '; '.join(reasons[:4])
            + ' and takes effect at the next start (docker compose down, then up)')
        install_home(prefix)
        return 'deferred'
    log('memory installation has to be re-prepared: ' + '; '.join(reasons[:4]))
    if running:
        raise Refused('the application is still running; stop it (docker compose down) and start again '
                      'so the memory installation can be updated')
    regenerated = None
    if policy_changes(recorded, desired):
        regenerated = parent / f'inputs-{role}.new'
        if regenerated.exists():
            shutil.rmtree(regenerated)
        write_inputs(role, regenerated, desired, health, recorded['node'], inputs)
    retired = ROOT / f'.retired-{role}'
    if retired.exists():
        shutil.rmtree(retired)
    if prefix.exists():
        prefix.rename(retired)
    try:
        prepare(role, prefix, regenerated or inputs, installed)
    except BaseException:
        if prefix.exists():
            shutil.rmtree(prefix)
        if retired.exists():
            retired.rename(prefix)
        if regenerated is not None:
            shutil.rmtree(regenerated, ignore_errors=True)
        raise
    switch_configuration(prefix, installed)
    if regenerated is not None:
        shutil.rmtree(inputs)
        regenerated.rename(inputs)
    install_home(prefix)
    if retired.exists():
        shutil.rmtree(retired)
    return 'update'


def main() -> int:
    role = os.environ.get('AIMEE_INSTANCE_ROLE', 'server')
    if role not in ('server', 'kb'):
        log(f'fatal: AIMEE_INSTANCE_ROLE must be server or kb, not {role!r}')
        return 2
    if os.geteuid() != 0:
        log('fatal: memory provisioning runs as container root')
        return 2
    try:
        outcome = provision(role, supplied_identity(b'' if sys.stdin.isatty() else sys.stdin.buffer.read(1 << 20)))
    except Refused as error:
        log(f'fatal: {error}')
        return 1
    log({'off': f'memory provisioning is off for {role}; the instance runs without memory unless an '
                'installation already exists',
         'fresh': f'memory installation created for {role}',
         'current': f'memory installation for {role} verified; nothing changed',
         'update': f'memory installation for {role} re-prepared; stored memory kept',
         'deferred': f'memory installation for {role} kept while the application runs'}[outcome])
    return 0


if __name__ == '__main__':
    sys.exit(main())
