#!/usr/bin/env python3
"""One-pass migration of hardcoded hex colours to smoothgui theme tokens.

The frontend styled everything with inline hex literals, so it had exactly one
palette and no way to render a dark one. smoothgui 0.11.0 ships the tokens and
flips them on `prefers-color-scheme` / `[data-theme]`, so the fix is to stop
naming colours and start naming ROLES.

Every colour is mapped explicitly below rather than by regex family: the same
hex means different things in different places, and a near-miss on a semantic
colour is a silent visual bug. Colours already used as dark surfaces (the header,
the nav, dark callout backgrounds) map to the dark/semantic-bg tokens that stay
dark in both themes, not to the light neutrals.

Run from frontend/:  python3 scripts/theme-migrate.py [--check]
"""
import pathlib
import re
import sys

# role -> token. Ordered longest-first at match time so #fff never eats #fff6e6.
MAP = {
    # ── neutral text scale ────────────────────────────────────────────────
    '#111': 'text', '#1a1a1a': 'text', '#222': 'text', '#223': 'text',
    '#233': 'text', '#333': 'text', '#334': 'text',
    '#444': 'text-muted', '#446': 'text-muted', '#456': 'text-muted',
    '#555': 'text-muted', '#556': 'text-muted',
    '#666': 'text-secondary', '#667': 'text-secondary',
    '#777': 'text-faint', '#778': 'text-faint', '#888': 'text-faint',
    '#889': 'text-faint', '#8a8a8a': 'text-faint', '#8899aa': 'text-faint',
    '#999': 'text-hint', '#99a': 'text-hint', '#9aa': 'text-hint',
    '#9ca3af': 'text-hint',
    '#aaa': 'text-pale', '#bbb': 'text-pale',

    # ── surfaces ──────────────────────────────────────────────────────────
    '#fff': 'surface',
    '#fafafa': 'surface-alt', '#fafbfc': 'surface-alt', '#fbfcfe': 'surface-alt',
    '#fbfdff': 'surface-alt', '#f8fafc': 'surface-alt', '#f8fbff': 'surface-alt',
    '#f6f9ff': 'surface-alt', '#f6f7f9': 'surface-alt', '#f4f7fb': 'surface-alt',
    '#f4f6fb': 'surface-alt', '#f4f4fa': 'surface-alt', '#f4f4f4': 'surface-alt',
    '#f3f3f3': 'surface-alt',
    '#f0f0f0': 'surface-active',
    '#f5f5f5': 'bg',
    '#eef': 'surface-sunken', '#eef0f4': 'surface-sunken', '#eef1f6': 'surface-sunken',
    '#eef2f7': 'surface-sunken', '#f1f4f9': 'surface-sunken',

    # ── borders ───────────────────────────────────────────────────────────
    '#e0e0e0': 'border', '#e2e2e2': 'border', '#e3e6ea': 'border',
    '#eee': 'border-light',
    '#ddd': 'border-medium', '#dde': 'border-medium', '#dfe6ef': 'border-medium',
    '#d5d9e0': 'border-medium', '#ccd3dc': 'border-medium', '#c9d1d9': 'border-medium',
    '#cbd5e1': 'border-medium', '#ccc': 'border-medium', '#ccd': 'border-medium',

    # ── dark chrome (header / nav): dark in BOTH themes ───────────────────
    '#13131f': 'dark-surface', '#0d1117': 'dark-surface', '#1a1a28': 'dark-surface',
    '#23233a': 'dark-surface-alt', '#3a3a55': 'dark-surface-alt',
    '#2a2a3a': 'dark-border',
    '#cde': 'sidebar-text',
    '#8cf': 'primary', '#9cf': 'primary', '#8bf': 'primary',

    # ── success ───────────────────────────────────────────────────────────
    '#1f7a3d': 'success-dark', '#2c8f56': 'success-dark', '#287a3f': 'success-dark',
    '#2c6f46': 'success-dark', '#2c5b3b': 'success-dark', '#22a06b': 'success-dark',
    '#2a7': 'success-dark', '#2c6': 'success-dark',
    '#22c55e': 'success', '#4caf50': 'success', '#4ec94e': 'success',
    '#7d7': 'success', '#8fd3a8': 'success', '#7bd9a2': 'success',
    '#8c8': 'success', '#8f8': 'success', '#7a7': 'success', '#adb': 'success',
    '#eaf6ee': 'success-bg', '#f1f7f1': 'success-bg', '#cfe2cf': 'success-bg',
    '#bfe0cb': 'success-bg',
    # already-dark green backgrounds
    '#1b3a26': 'success-bg', '#193226': 'success-bg', '#1e2a1e': 'success-bg',
    '#1a2a1a': 'success-bg', '#2d4d2d': 'success-bg',

    # ── danger ────────────────────────────────────────────────────────────
    '#c00': 'danger-dark', '#b00': 'danger-dark', '#a51d1d': 'danger-dark',
    '#c62828': 'danger-dark', '#a15': 'danger-dark', '#844': 'danger-dark',
    '#a33': 'danger', '#c33': 'danger', '#c66': 'danger', '#d99': 'danger',
    '#e88': 'danger', '#d4564f': 'danger', '#ef4444': 'danger',
    '#f87171': 'danger-light', '#ff6b6b': 'danger-light',
    '#fdeaea': 'danger-bg', '#fff3f3': 'danger-bg', '#fff7f7': 'danger-bg',
    '#ffd2d2': 'danger-bg', '#f2c4c4': 'danger-bg', '#e0b4b4': 'danger-bg',
    '#fee': 'danger-bg',
    # already-dark red backgrounds
    '#5a2a2a': 'danger-bg', '#3a1a1a': 'danger-bg',

    # ── warning ───────────────────────────────────────────────────────────
    '#8a5a00': 'warning-dark', '#a67c00': 'warning-dark', '#9a6700': 'warning-dark',
    '#b26a00': 'warning-dark', '#a60': 'warning-dark', '#6b4423': 'warning-dark',
    '#a96': 'warning-dark', '#775': 'warning-dark',
    '#e0a800': 'warning', '#f39c12': 'warning', '#f59e0b': 'warning',
    '#f4b860': 'warning', '#f0c36a': 'warning', '#f0c088': 'warning',
    '#ffd93d': 'warning', '#f2d06b': 'warning', '#d8b48a': 'warning',
    '#f0d9a8': 'warning-border', '#f0d8a8': 'warning-border',
    '#f0dca8': 'warning-border', '#ffd9a0': 'warning-border',
    '#fff6e6': 'warning-bg', '#fff8e6': 'warning-bg',
    # already-dark amber backgrounds
    '#6a4a1a': 'warning-bg', '#3a3017': 'warning-bg', '#3a2a12': 'warning-bg',
    '#3a2416': 'warning-bg', '#4a4a2a': 'warning-bg', '#2a2a1a': 'warning-bg',

    # ── info / blues ──────────────────────────────────────────────────────
    '#2563eb': 'info', '#3b82f6': 'info', '#0ea5e9': 'info', '#3a6ea5': 'info',
    '#68a': 'info',
    '#0a58ca': 'info-dark',
    '#9ec7ff': 'info-border', '#7c9ef8': 'info-border', '#a5b4fc': 'info-border',
    '#bcd4ff': 'info-border', '#cbd8eb': 'info-border', '#cdf': 'info-border',
    '#dbe5f4': 'info-border', '#d9e2ef': 'info-border',
    '#eef4ff': 'info-bg', '#e8eef9': 'info-bg', '#f1f6fd': 'info-bg',
    '#eef2ff': 'info-bg', '#e0e7ff': 'info-bg',
    # already-dark blue backgrounds
    '#1a3a5c': 'info-bg', '#1f2b3a': 'info-bg',

    # ── purple ────────────────────────────────────────────────────────────
    '#8b5cf6': 'purple', '#6366f1': 'purple', '#4f46e5': 'purple',
    # muted mauve label accent marking "custom" pipeline stages
    '#a67': 'purple',
}

ROOT = pathlib.Path('src')
# Longest literal first so #fff6e6 is matched before #fff.
PATTERN = re.compile(
    '|'.join(re.escape(k) for k in sorted(MAP, key=len, reverse=True)),
    re.IGNORECASE,
)


def main() -> int:
    check = '--check' in sys.argv
    files = sorted(p for ext in ('*.tsx', '*.ts') for p in ROOT.rglob(ext))
    total, touched, unmapped = 0, 0, {}

    for path in files:
        src = path.read_text()
        # Don't rewrite colours inside test fixtures asserting literal output.
        n = 0

        def sub(m):
            nonlocal n
            n += 1
            return f'var(--sg-{MAP[m.group(0).lower()]})'

        out = PATTERN.sub(sub, src)
        if n:
            total += n
            touched += 1
            if not check:
                path.write_text(out)
        for leftover in re.findall(r'#[0-9a-fA-F]{3,8}\b', out):
            unmapped[leftover.lower()] = unmapped.get(leftover.lower(), 0) + 1

    print(f'{"would replace" if check else "replaced"} {total} colours in {touched} files')
    if unmapped:
        print(f'UNMAPPED ({sum(unmapped.values())} occurrences, {len(unmapped)} distinct):')
        for c, k in sorted(unmapped.items(), key=lambda kv: -kv[1]):
            print(f'  {c} x{k}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
