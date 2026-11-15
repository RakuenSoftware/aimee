#!/usr/bin/env python3
"""Verify the vendored memory bundle against this tree, and unpack it for a build.

`vendor/aims-memory/bundle.json` records, for each platform, the archive's
SHA-256 and a fingerprint of every public file the prebuilt bridge objects were
compiled against. Those objects embed the layout of public structures, so a
bundle is valid only while the fingerprinted files are byte-identical. This
check fails, naming the changed files, instead of letting a stale bundle link.

    check_memory_bundle.py                      verify every recorded platform
    check_memory_bundle.py --platform P --unpack DIR
                                                verify P, extract it into DIR and
                                                verify every extracted file
    check_memory_bundle.py --record ARCHIVE     write bundle.json for a new archive
                                                (maintainers, after a bundle rebuild)

Standard library only; run with `python3 -I`.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = Path('vendor/aims-memory')
INDEX = 'bundle.json'
REQUIRED = ('bin/aims-db-module', 'bin/aimee-memory-module', 'bin/memory-profile',
            'bin/aims-overlay-schema-bootstrap', 'bin/aims-db-owner-bootstrap',
            'lib/libaims-overlay-server.a', 'lib/libaims-overlay-kb.a', 'lib/libaimee_original_input_coverage.a',
            'link/overlay-link.mk', 'provision/operator_inputs.py', 'provision/prepare.py', 'provision/verify.py',
            'provision/compose_install_home.py')


class Stale(Exception):
    """The bundle does not belong to this tree."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_index(root: Path) -> dict:
    path = root / VENDOR / INDEX
    if not path.is_file():
        raise Stale(f'{VENDOR / INDEX} is missing')
    index = json.loads(path.read_text())
    if index.get('schema_version') != 1 or not isinstance(index.get('platforms'), dict) or not index['platforms']:
        raise Stale(f'{VENDOR / INDEX} is not a version 1 bundle index')
    return index


def check_platform(root: Path, platform: str, entry: dict) -> bytes:
    """Return the verified archive bytes of one platform."""
    archive = root / VENDOR / entry['archive']
    if not archive.is_file():
        raise Stale(f'{platform}: {VENDOR / entry["archive"]} is missing')
    data = archive.read_bytes()
    if sha256(data) != entry['sha256']:
        raise Stale(f'{platform}: archive does not match the SHA-256 recorded in {INDEX}')
    headers = entry.get('public_headers')
    if not isinstance(headers, dict) or not headers:
        raise Stale(f'{platform}: {INDEX} records no public header fingerprint')
    changed = []
    for relative, digest in sorted(headers.items()):
        path = root / relative
        if not path.is_file():
            changed.append(f'{relative} (removed)')
        elif sha256(path.read_bytes()) != digest:
            changed.append(relative)
    if changed:
        raise Stale(f'{platform}: the bundle was compiled against different versions of {len(changed)} public '
                    'file(s); rebuild the memory bundle for this tree:\n  ' + '\n  '.join(changed))
    return data


def members(data: bytes, platform: str):
    """Regular files of the archive as (relative path, mode, bytes); anything else is refused."""
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:xz') as archive:
        for member in archive:
            parts = Path(member.name).parts
            if not parts or parts[0] != platform or '..' in parts or member.name.startswith('/'):
                raise Stale(f'{platform}: unexpected archive member {member.name!r}')
            if member.isdir():
                continue
            if not member.isreg():
                raise Stale(f'{platform}: archive member {member.name!r} is not a regular file')
            yield str(Path(*parts[1:])), member.mode, archive.extractfile(member).read()


def unpack(data: bytes, platform: str, entry: dict, destination: Path) -> int:
    if destination.exists() and any(destination.iterdir()):
        raise Stale(f'{destination} is not empty')
    files = {relative: (mode, content) for relative, mode, content in members(data, platform)}
    manifest = json.loads(files['MANIFEST.json'][1]) if 'MANIFEST.json' in files else None
    if not manifest or manifest.get('contains_source') is not False or manifest.get('platform') != platform:
        raise Stale(f'{platform}: archive manifest is missing or is not a source-free {platform} bundle')
    if manifest.get('public_headers') != entry['public_headers']:
        raise Stale(f'{platform}: archive manifest and {INDEX} record different header fingerprints')
    recorded = manifest.get('files', {})
    if set(recorded) != set(files) - {'MANIFEST.json'}:
        raise Stale(f'{platform}: archive contents differ from its manifest')
    for relative, digest in recorded.items():
        if sha256(files[relative][1]) != digest:
            raise Stale(f'{platform}: {relative} does not match the archive manifest')
    for relative in REQUIRED:
        if relative not in files:
            raise Stale(f'{platform}: bundle lacks {relative}')
    for relative, (mode, content) in files.items():
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o755 if mode & 0o100 else 0o644)
    return len(files)


def record(root: Path, archive: Path) -> dict:
    """Index entry for a freshly built archive; the header fingerprint comes from its own manifest."""
    platform = archive.name.removesuffix('.tar.xz')
    data = archive.read_bytes()
    manifest = None
    for relative, _, content in members(data, platform):
        if relative == 'MANIFEST.json':
            manifest = json.loads(content)
    if manifest is None:
        raise Stale(f'{archive} has no MANIFEST.json')
    return {platform: {'archive': archive.name, 'sha256': sha256(data), 'bytes': len(data),
                       'aims_db_commit': manifest['aims_db_commit'], 'aimee_commit': manifest['aimee_commit'],
                       'public_headers': manifest['public_headers']}}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--platform')
    parser.add_argument('--unpack', type=Path, metavar='DIR')
    parser.add_argument('--record', type=Path, metavar='ARCHIVE')
    args = parser.parse_args(argv)
    try:
        if args.record:
            path = args.root / VENDOR / INDEX
            index = json.loads(path.read_text()) if path.is_file() else {'schema_version': 1, 'platforms': {}}
            index['platforms'].update(record(args.root, args.record))
            path.write_text(json.dumps(index, indent=2, sort_keys=True) + '\n')
            print(f'recorded {args.record.name} in {VENDOR / INDEX}')
            return 0
        index = load_index(args.root)
        if args.unpack and not args.platform:
            parser.error('--unpack needs --platform')
        selected = [args.platform] if args.platform else sorted(index['platforms'])
        for platform in selected:
            entry = index['platforms'].get(platform)
            if entry is None:
                raise Stale(f'{platform}: no bundle is recorded for this platform')
            data = check_platform(args.root, platform, entry)
            if args.unpack:
                count = unpack(data, platform, entry, args.unpack)
                print(f'{platform}: unpacked {count} verified files into {args.unpack}')
            else:
                print(f'{platform}: archive and {len(entry["public_headers"])} public file fingerprints match')
    except Stale as error:
        print(f'check_memory_bundle: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
