# Vendored frontend dependencies

`@rakuensoftware/smoothgui` is published to **GitHub Packages**, whose npm
registry requires an auth token even for public reads. To keep the webchat image
buildable with **no credentials** — so anyone can `docker pull` (or build from
source) and get the browser UI by default — the package is vendored here as a
tarball and referenced from `frontend/package.json` via a `file:` dependency.

## Refreshing to a new smoothgui version

From a checkout of the smoothgui repo (or an installed copy of the target
version):

```bash
# 1. Build + pack the desired version (produces rakuensoftware-smoothgui-<ver>.tgz)
cd /path/to/smoothgui
npm ci && npm run build
npm pack --pack-destination /path/to/aimee/frontend/vendor

# 2. Point package.json at the new tarball and drop the old one
cd /path/to/aimee/frontend
rm vendor/rakuensoftware-smoothgui-<old>.tgz
#   edit package.json:
#   "@rakuensoftware/smoothgui": "file:vendor/rakuensoftware-smoothgui-<new>.tgz"

# 3. Regenerate the lockfile and verify the SPA still builds
rm -rf node_modules package-lock.json
npm install
npm ci && npm run build      # this is exactly what the Docker frontend stage runs
```

Commit the new `.tgz`, `package.json`, and `package-lock.json` together.
`node_modules/` and `dist/` stay gitignored.
