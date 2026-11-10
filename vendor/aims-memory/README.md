# Vendored memory bundle

Aimee's memory implementation is built from a separate, private repository
(`RakuenSoftware/aims-db`). This directory vendors its build output so the
release image ships working memory without that source being present here.

| File | Contents |
| --- | --- |
| `linux-amd64.tar.xz` | The bundle for one platform (about 25 MB; 149 MB unpacked). |
| `bundle.json` | The archive's SHA-256, the two source commits it was built from, and a fingerprint of every public file its objects were compiled against. |

The archive contains no source of the private repository:

- `bin/` — the memory database, memory module, profile, schema bootstrap and
  owner bootstrap executables (stripped);
- `lib/` — static archives of the bridge objects linked into `aimee-server` and
  `aimee-kb`, and the input-screening archive;
- `link/overlay-link.mk` — link flags only;
- `provision/` — the Python scripts that provision an installation at first
  boot (generated grants, configuration and owner key);
- `MANIFEST.json` — a hash of every file above.

## How it is used

`Dockerfile.server` verifies the bundle with `scripts/check_memory_bundle.py`,
unpacks it, links the two resources against `lib/`, and copies `bin/` and
`provision/` into the image under `/opt/aimee/memory`. At first boot the
`aimee-memory-init` Compose service runs `aimee-memory-provision`
(`deploy/container/memory-provision.py`), which turns the bundle into this
deployment's installation. See [Deployment](../../docs/DEPLOYMENT.md).

A platform without an archive here builds an image without memory.

## Staleness

The bridge objects embed the layout of public structures. The bundle is valid
only while the files fingerprinted in `bundle.json` are byte-identical to this
tree. `make -C src lint` runs `scripts/check_memory_bundle.py`, which fails and
names the changed files when they are not; the image build runs the same check
and refuses to link a stale bundle.

A change to any fingerprinted file therefore needs a rebuilt bundle before it
can merge. A maintainer with access to the private repository rebuilds it:

```sh
# in a checkout of the private repository, on a Docker host
packaging/aimee-overlay/reproduce_composition.sh <aims-db> <aimee> <work>
packaging/aimee-overlay/build_vendor_bundle.py --work <work> \
    --aims-source <aims-db> --aimee-source <aimee> --output <out>
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
    -C <out> -cf - linux-amd64 | xz -9e > vendor/aims-memory/linux-amd64.tar.xz

# in this repository
python3 -I scripts/check_memory_bundle.py --record vendor/aims-memory/linux-amd64.tar.xz
python3 -I scripts/check_memory_bundle.py
```

## Licensing

The files in the archive are binary and script output of the private
repository. They are not source of this project, and this repository's licence
file does not by itself describe the terms under which they are distributed;
those are set by their copyright holder.
