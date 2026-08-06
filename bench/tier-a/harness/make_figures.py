#!/usr/bin/env python3
"""Turn an article's markdown tables into chart-or-numbers figures.

The output is smoothgui's DataFigure markup: a `sg-figure` with two radio
inputs, a chart pane and a numbers pane. The switch is CSS only, because the
site renders posts through `marked` into `Prose`, which injects HTML with
dangerouslySetInnerHTML — script never executes there.

Nothing here carries colour. Marks take the `sg-chart` classes and inherit ink,
gridlines and series colour from smoothgui's tokens, so a figure follows the
site's theme instead of freezing whatever was correct the day it was generated.
That is why an earlier hand-rolled version was wrong: it shipped its own
`prefers-color-scheme` block, which fires on the reader's OS preference even
where the surface underneath it never changes.

Usage:
    python3 harness/make_figures.py articles/v2/00-the-head-to-head.md

Idempotent: refuses to run twice on the same file.
"""
import re
import sys

MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"


# ---------------------------------------------------------------- md helpers
def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def md_inline(s):
    s = esc(s)
    s = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', s)
    s = re.sub(r'`(.+?)`', r'<code>\1</code>', s)
    return s


def cells(line):
    return [c.strip() for c in line.strip().strip('|').split('|')]


def num(s):
    return float(re.sub(r'[^\d.\-]', '', s.replace('−', '-')))


def table_html(lines):
    head = cells(lines[0])
    align = ['right' if a.endswith(':') and not a.startswith(':') else 'left'
             for a in cells(lines[1])]
    out = ['<table><thead><tr>']
    for i, h in enumerate(head):
        out.append('<th style="text-align:%s">%s</th>' % (align[i], md_inline(h)))
    out.append('</tr></thead><tbody>')
    for l in lines[2:]:
        out.append('<tr>')
        for i, c in enumerate(cells(l)):
            out.append('<td style="text-align:%s">%s</td>'
                       % (align[i] if i < len(align) else 'left', md_inline(c)))
        out.append('</tr>')
    out.append('</tbody></table>')
    return ''.join(out)


def svg(w, h, label, body):
    return ('<svg class="sg-chart" viewBox="0 0 %d %d" preserveAspectRatio="xMidYMid meet" '
            'role="img" aria-label="%s">%s</svg>' % (w, h, esc(label), ''.join(body)))


def series_class(kind, mark=True):
    """kind is 1, 2 or None for de-emphasised."""
    base = 'sg-chart__mark' if mark else 'sg-chart__line'
    return '%s %s--%s' % (base, base, kind if kind else 'muted')


# ---------------------------------------------------------------- chart forms
def chart_ranked(rows, value_col, label_cols, hi, accent):
    """Horizontal lollipops, one row per arm, ranked as given."""
    W, rh, padT, padB, gut, padR = 760, 15, 24, 34, 224, 62
    H = padT + len(rows) * rh + padB
    def x(v):
        return gut + (v / hi) * (W - gut - padR)
    b = []
    for t in [hi * f for f in (0.25, 0.5, 0.75)]:
        b.append('<line class="sg-chart__grid" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/>'
                 % (x(t), x(t), padT - 8, padT + len(rows) * rh + 4))
        b.append('<text class="sg-chart__value" x="%.1f" y="%d" text-anchor="middle" '
                 'opacity=".7">%s</text>' % (x(t), padT + len(rows) * rh + 20, ('%g' % round(t, 2))))
    for i, r in enumerate(rows):
        cy = padT + i * rh + rh / 2
        v = num(r[value_col])
        name = ' '.join(r[c] for c in label_cols)
        k = accent(r)
        b.append('<text class="sg-chart__label" x="%d" y="%.1f" text-anchor="end" '
                 'font-size="11">%s</text>' % (gut - 10, cy + 3.5, esc(name)[:38]))
        b.append('<line class="%s" x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/>'
                 % (series_class(k, mark=False), x(0), x(v), cy, cy))
        b.append('<circle class="%s" cx="%.1f" cy="%.1f" r="3.6"/>'
                 % (series_class(k), x(v), cy))
        b.append('<text class="sg-chart__value" x="%.1f" y="%.1f">%.4f</text>'
                 % (x(v) + 8, cy + 3.5, v))
    return svg(W, H, 'Ranked values, one row per arm', b)


def chart_intervals(rows, label):
    """Point estimate with a 95% interval, against a zero rule."""
    W, rh, padT, padB, gut, padR = 760, 30, 40, 40, 190, 80
    H = padT + len(rows) * rh + padB
    los = [num(r[2].split(',')[0]) for r in rows]
    his = [num(r[2].split(',')[1]) for r in rows]
    d0, d1 = min(los) - 0.012, max(his) + 0.012
    def x(v):
        return gut + (v - d0) / (d1 - d0) * (W - gut - padR)
    yb = padT + len(rows) * rh
    b = []
    for t in [t / 100 for t in range(-8, 14, 4)]:
        if not (d0 <= t <= d1):
            continue
        cls = 'sg-chart__rule' if t == 0 else 'sg-chart__grid'
        b.append('<line class="%s" x1="%.1f" x2="%.1f" y1="%d" y2="%.1f"/>'
                 % (cls, x(t), x(t), padT - 14, yb + 2))
        lab = '0' if abs(t) < 1e-9 else ('+' if t > 0 else '−') + ('%.2f' % abs(t))
        b.append('<text class="sg-chart__value" x="%.1f" y="%.1f" text-anchor="middle" '
                 'opacity=".7">%s</text>' % (x(t), yb + 20, lab))
    if d0 <= 0 <= d1:
        b.append('<text class="sg-chart__axis" x="%.1f" y="%d" text-anchor="middle">'
                 'NO DIFFERENCE</text>' % (x(0), padT - 22))
    for i, r in enumerate(rows):
        cy = padT + i * rh + rh / 2
        d = num(r[1])
        lo, hi = num(r[2].split(',')[0]), num(r[2].split(',')[1])
        sig = lo > 0 or hi < 0
        k = 1 if sig else None
        b.append('<text class="sg-chart__label" x="%d" y="%.1f" text-anchor="end">%s</text>'
                 % (gut - 14, cy + 4, esc(r[0])))
        b.append('<line class="%s" x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/>'
                 % (series_class(k, mark=False), x(lo), x(hi), cy, cy))
        for v in (lo, hi):
            b.append('<line class="%s" x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/>'
                     % (series_class(k, mark=False), x(v), x(v), cy - 4.5, cy + 4.5))
        b.append('<circle class="%s sg-chart__ring" cx="%.1f" cy="%.1f" r="4.5"/>'
                 % (series_class(k), x(d), cy))
        b.append('<text class="sg-chart__value" x="%d" y="%.1f"%s>%s</text>'
                 % (W - padR + 12, cy + 4, '' if sig else ' opacity=".7"',
                    ('+' if d >= 0 else '−') + '%.4f' % abs(d)))
    return svg(W, H, label, b)


def chart_bars(rows, value_col, label_col, note_col, hi, unit, accent):
    W, rh, padT, padB, gut, padR = 760, 34, 24, 36, 210, 128
    H = padT + len(rows) * rh + padB
    def x(v):
        return gut + (v / hi) * (W - gut - padR)
    b = []
    for t in [hi * f for f in (0.3, 0.6, 0.9)]:
        b.append('<line class="sg-chart__grid" x1="%.1f" x2="%.1f" y1="%d" y2="%d"/>'
                 % (x(t), x(t), padT - 8, padT + len(rows) * rh + 4))
        b.append('<text class="sg-chart__value" x="%.1f" y="%d" text-anchor="middle" '
                 'opacity=".7">%d</text>' % (x(t), padT + len(rows) * rh + 20, round(t)))
    for i, r in enumerate(rows):
        cy = padT + i * rh + rh / 2
        v = num(r[value_col])
        k = accent(r)
        b.append('<text class="sg-chart__label" x="%d" y="%.1f" text-anchor="end">%s</text>'
                 % (gut - 12, cy + 4, esc(r[label_col])))
        b.append('<rect class="%s" x="%.1f" y="%.1f" width="%.1f" height="9" rx="4"/>'
                 % (series_class(k), x(0), cy - 4.5, max(x(v) - x(0), 1)))
        b.append('<text class="sg-chart__value" x="%.1f" y="%.1f">%.1f</text>'
                 % (x(v) + 9, cy + 4, v))
        if note_col is not None:
            b.append('<text class="sg-chart__value" x="%d" y="%.1f" opacity=".7">%s</text>'
                     % (W - 74, cy + 4, esc(r[note_col])))
    b.append('<text class="sg-chart__axis" x="%.1f" y="%d" text-anchor="middle">%s</text>'
             % ((gut + W - padR) / 2, H - 6, unit))
    return svg(W, H, 'Magnitude per arm', b)


def chart_paired(rows, metrics):
    """Two arms across several metrics, each metric on its own scale.

    Deliberately not one shared axis: the metrics have different units, and a
    shared scale would invent a comparison the data does not support.
    """
    W, blockH, padT, gut, padR = 760, 62, 24, 178, 150
    H = padT + len(metrics) * blockH + 20
    names = [rows[0][0], rows[1][0]]
    b = []
    for mi, (label, col, scale, fmt) in enumerate(metrics):
        top = padT + mi * blockH
        b.append('<text class="sg-chart__axis" x="0" y="%d">%s</text>' % (top + 8, esc(label.upper())))
        for j in (0, 1):
            v = num(rows[j][col])
            cy = top + 24 + j * 17
            w = (v / scale) * (W - gut - padR)
            b.append('<text class="sg-chart__label" x="%d" y="%.1f" text-anchor="end" '
                     'font-size="11">%s</text>' % (gut - 10, cy + 4, esc(names[j])))
            b.append('<rect class="%s" x="%d" y="%.1f" width="%.1f" height="9" rx="4"/>'
                     % (series_class(1 if j == 0 else 2), gut, cy - 4.5, max(w, 1)))
            b.append('<text class="sg-chart__value" x="%.1f" y="%.1f">%s</text>'
                     % (gut + w + 9, cy + 4, fmt % v))
    return svg(W, H, 'Two arms across several metrics, each on its own scale', b)


def chart_scatter(rows, xcol, ycol, xhi, yhi, xlab, ylab, accent, labelled):
    """Two measures against each other, to show whether they move together."""
    W, H, L, R, T, B = 760, 440, 58, 30, 34, 52
    def x(v):
        return L + (v / xhi) * (W - L - R)
    def y(v):
        return H - B - (v / yhi) * (H - T - B)
    b = []
    for t in [yhi * f / 6 for f in range(7)]:
        b.append('<line class="sg-chart__grid" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>'
                 % (L, W - R, y(t), y(t)))
        b.append('<text class="sg-chart__value" x="%d" y="%.1f" text-anchor="end" '
                 'opacity=".7">%d</text>' % (L - 10, y(t) + 4, round(t)))
    for t in [xhi * f / 7 for f in range(1, 8)]:
        b.append('<text class="sg-chart__value" x="%.1f" y="%d" text-anchor="middle" '
                 'opacity=".7">%.1f</text>' % (x(t), H - B + 20, t))
    b.append('<text class="sg-chart__axis" x="%.1f" y="%d" text-anchor="middle">%s</text>'
             % ((L + W - R) / 2, H - 12, xlab))
    b.append('<text class="sg-chart__axis" x="%d" y="18">%s</text>' % (L - 46, ylab))
    for r in rows:
        k = accent(r)
        b.append('<circle class="%s sg-chart__ring" cx="%.1f" cy="%.1f" r="%s">'
                 '<title>%s %s</title></circle>'
                 % (series_class(k), x(num(r[xcol])), y(num(r[ycol])), '6' if k else '5',
                    esc(r[0]), esc(r[1])))
    for name, quant, dx, dy, anch in labelled:
        hit = next((r for r in rows if r[0] == name and (quant is None or r[1] == quant)), None)
        if not hit:
            continue
        b.append('<text class="sg-chart__label" x="%.1f" y="%.1f" text-anchor="%s">%s</text>'
                 % (x(num(hit[xcol])) + dx, y(num(hit[ycol])) + dy, anch, esc(name)))
    return svg(W, H, '%s against %s' % (xlab.lower(), ylab.lower()), b)


def standalone(fid, chart, caption, key=None):
    """A figure with no numbers pane — the table it derives from lives elsewhere."""
    return ('<figure class="sg-figure" id="%s">%s%s'
            '<figcaption class="sg-figure__caption">%s</figcaption></figure>'
            % (fid, chart, key or '', caption))


# ---------------------------------------------------------------- assembly
def legend(items):
    return ('<div class="sg-figure__legend">'
            + ''.join('<span><i style="background:var(--sg-chart-%s)"></i>%s</span>'
                      % (k, esc(t)) for k, t in items)
            + '</div>')


def figure(fid, chart, table, caption, key=None):
    return (
        '<figure class="sg-figure">'
        '<input class="sg-figure__radio sg-figure__radio--chart" type="radio" name="%s" id="%s-chart" checked>'
        '<input class="sg-figure__radio sg-figure__radio--table" type="radio" name="%s" id="%s-table">'
        '<div class="sg-figure__tabs">'
        '<label class="sg-figure__tab sg-figure__tab--chart" for="%s-chart">Chart</label>'
        '<label class="sg-figure__tab sg-figure__tab--table" for="%s-table">Numbers</label>'
        '</div>'
        '<div class="sg-figure__panes">'
        '<div class="sg-figure__pane sg-figure__pane--chart">%s%s</div>'
        '<div class="sg-figure__pane sg-figure__pane--table">%s</div>'
        '</div>'
        '<figcaption class="sg-figure__caption">%s</figcaption>'
        '</figure>' % (fid, fid, fid, fid, fid, fid, chart, key or '', table, caption)
    )


def find_tables(lines):
    out, i = [], 0
    while i < len(lines):
        if lines[i].startswith('|') and i + 1 < len(lines) and re.match(r'^\|[-: |]+\|$', lines[i + 1]):
            j = i
            while j < len(lines) and lines[j].startswith('|'):
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def main(path):
    src = open(path).read()
    if 'sg-figure' in src:
        sys.exit('already has figures: %s' % path)

    lines = src.split('\n')
    blocks = find_tables(lines)
    if not blocks:
        sys.exit('no tables found in %s' % path)

    top2 = ('Qwen3.6-35B-A3B', 'Qwen3.6-27B dense')

    def rank_accent(r):
        if r[0] in top2:
            return 1
        if r[0] == 'granite-4.1-3b':
            return 2
        return None

    spec = {
        0: dict(kind='ranked', value=2, labels=(0, 1), hi=0.78, accent=rank_accent,
                cap='Every arm on the corpus, ranked by strict F1. '
                    'Switch to Numbers for the full metric set.',
                key=[('1', 'separable top'), ('2', 'lowest invention'),
                     ('muted', 'other arms')]),
        1: dict(kind='intervals',
                cap='Difference in strict F1 between each neighbouring pair, with a 95% '
                    'interval. An interval crossing zero means the ranking could be either '
                    'way round.',
                key=[('1', 'interval excludes zero'), ('muted', 'interval contains zero')]),
        2: dict(kind='intervals',
                cap='The same stretch measured end to end rather than rung by rung.',
                key=[('1', 'interval excludes zero')]),
        3: dict(kind='bars', value=1, label=0, note=2, hi=340.0, unit='TOKENS PER SECOND',
                accent=lambda r: 1 if num(r[1]) >= 300 else None,
                cap='Sustained throughput per arm, on the card each one ran on.'),
        4: dict(kind='paired',
                metrics=[('F1', 1, 0.9, '%.4f'), ('recall', 2, 0.9, '%.4f'),
                         ('abstains on factless', 3, 1.0, '%.3f'),
                         ('invented triples', 4, 200.0, '%.0f')],
                cap='Two arms that F1 cannot separate, across the columns that do separate '
                    'them. Each metric is drawn on its own scale, because the units differ.'),
    }

    built = 0
    out = list(lines)
    for bi, (a, b) in reversed(list(enumerate(blocks))):
        s = spec.get(bi)
        if not s:
            continue  # not every table is chart-shaped; leave it as a table
        tbl = lines[a:b]
        rows = [cells(l) for l in tbl[2:]]
        if s['kind'] == 'ranked':
            chart = chart_ranked(rows, s['value'], s['labels'], s['hi'], s['accent'])
        elif s['kind'] == 'intervals':
            chart = chart_intervals(rows, 'Paired differences with 95% intervals')
        elif s['kind'] == 'bars':
            chart = chart_bars(rows, s['value'], s['label'], s['note'], s['hi'],
                               s['unit'], s['accent'])
        else:
            chart = chart_paired(rows, s['metrics'])
        key = legend(s['key']) if s.get('key') else None
        out[a:b] = [figure('fig-%d' % bi, chart, table_html(tbl), s['cap'], key)]
        built += 1

    text = '\n'.join(out)

    # The scatter is not a table's counterpart — it is the argument that F1 and
    # invention are independent axes, which a table sorted by F1 conceals. It
    # goes where that claim is made, with no numbers pane, because its rows are
    # the main table's.
    anchor = 'So the decision is about what your pipeline does with a wrong fact.'
    if anchor in text:
        arms = [cells(l) for l in lines[blocks[0][0]:blocks[0][1]][2:]]
        sc = chart_scatter(
            arms, 2, 7, 0.79, 300.0, 'STRICT F1', 'INVENTED TRIPLES', rank_accent,
            [('Qwen3.6-35B-A3B', None, 11, -9, 'start'),
             ('Qwen3.6-27B dense', None, 11, 5, 'start'),
             ('gemma-4-31B', 'QAT UD-Q4', 11, 5, 'start'),
             ('granite-4.1-3b', None, -11, 4, 'end')])
        fig = standalone(
            'fig-scatter', sc,
            'Every arm by its F1 and by how many triples it invents on the 322 notes whose '
            'correct answer is an empty list. Two arms can score the same and write very '
            'different amounts of false data downstream.',
            legend([('1', 'separable top'), ('2', 'lowest invention'), ('muted', 'other arms')]))
        text = text.replace(anchor, fig + '\n\n' + anchor, 1)
        built += 1

    open(path, 'w').write(text)
    print('tables: %d | figures: %d | left as tables: %d'
          % (len(blocks), built, len(blocks) - built + 1))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
