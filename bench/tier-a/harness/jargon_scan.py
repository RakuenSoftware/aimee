#!/usr/bin/env python3
"""Count terms of art in an article's prose, and show where each is first used.

Written after reader feedback that a published piece was too jargony. Density
turned out not to be the fault: 3.7% of words, which is unremarkable. The fault
was that six load-bearing terms were never introduced at all, three of them in
the title and the first sentence, and the piece used F1 eight times without once
saying what F1 is.

So the useful output is the first-use list, not the count. Read it and ask of
each line: would a reader who cannot expand this term still be able to check the
claim the sentence makes?

The rule it serves is in VOICE.md under clarity and brevity: a term of art is
expanded on first use, or replaced.

Usage:
    python3 harness/jargon_scan.py articles/v2/00-the-head-to-head.md
"""
import collections
import re
import sys

TERMS = [
    'paired bootstrap', 'bootstrap', 'adjacent pair', 'confidence interval', '95% CI',
    'interval', 'strict F1', 'F1', 'precision', 'recall', 'indistinguishable',
    'separable', 'significant', 'null', 'stratum', 'strata',
    'subject-relation-object', 'triple', 'abstention', 'abstain', 'spurious',
    'relation-agnostic', 'ontology', 'predicate', 'corpus', 'arm', 'quant',
    'QAT', 'MoE', 'UD-Q4', 'mixture of experts', 'parse rate', 'reasoning pass',
    'speculative', 'token', 'VRAM', 'generator artefact',
]


def prose_of(path):
    """Article text with tables, generated figures and code blocks removed."""
    out, in_code = [], False
    for l in open(path):
        l = l.rstrip('\n')
        if l.startswith('```'):
            in_code = not in_code
            continue
        if in_code or l.startswith('|') or l.startswith('<') or l.startswith('    '):
            continue
        out.append(l)
    return '\n'.join(out)


def main(path):
    text = prose_of(path)
    counts = collections.Counter()
    for t in TERMS:
        n = len(re.findall(r'\b' + re.escape(t) + r'\b', text, re.I))
        if n:
            counts[t] = n

    words = len(re.findall(r'\w+', text))
    total = sum(counts.values())
    print('prose words: %d' % words)
    print('term hits  : %d (%.1f%% of words)\n' % (total, 100 * total / max(words, 1)))
    for t, n in counts.most_common(24):
        print('%4d  %s' % (n, t))

    print('\n--- first use, in order of appearance ---')
    firsts = []
    for t in counts:
        m = re.search(r'\b' + re.escape(t) + r'\b', text, re.I)
        if m:
            firsts.append((m.start(), t))
    for pos, t in sorted(firsts):
        ctx = text[max(0, pos - 60):pos + 70].replace('\n', ' ').strip()
        print('%-24s …%s…' % (t, ctx))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'articles/v2/00-the-head-to-head.md')
