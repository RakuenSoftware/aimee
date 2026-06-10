#!/usr/bin/env python3
"""guardrails-semantic.py: Reference sidecar for neural-assisted guardrails.

Reads a charter-envelope JSON request from stdin, returns risk scores as JSON.
Uses heuristic scoring only (no ML model calls).
"""
import json
import re
import sys

ACTION_RISK_WRITE_KEYWORDS = {'rm', 'mv', 'dd', 'truncate', 'chmod', 'chown', 'sudo', 'unlink'}
ACTION_RISK_EDIT_TOOLS = {'Edit', 'Write', 'MultiEdit', 'WriteEdit'}
ACTION_RISK_READ_TOOLS = {'Read', 'ReadMultiple'}

VERIFICATION_BYPASS_PATTERNS = [
    re.compile(r'\bif\s*\(\s*\w*ok\w*', re.IGNORECASE),
    re.compile(r'\brun_tests?\b', re.IGNORECASE),
    re.compile(r'\bverify\b', re.IGNORECASE),
    re.compile(r'\bassert\b', re.IGNORECASE),
    re.compile(r'\bcheck\b.*?(?:ok|pass)', re.IGNORECASE),
]

SECRET_LEAK_PATTERNS = [
    re.compile(r'\bpassword\s*=', re.IGNORECASE),
    re.compile(r'\bsecret\s*=', re.IGNORECASE),
    re.compile(r'\btoken\s*=', re.IGNORECASE),
    re.compile(r'\bkey\s*=\s*["\'][^"\']{8,}', re.IGNORECASE),
    re.compile(r'api[_-]?key\s*=', re.IGNORECASE),
    re.compile(r'private[_-]?key\s*=', re.IGNORECASE),
]

# Anti-pattern categories for criterion #7 (pattern similarity clustering).
# Each entry: (category_name, [phrase_patterns]).  A category matches when ≥ 2 patterns fire.
ANTIPATTERN_CATEGORIES = [
    ('disable_auth', [
        re.compile(r'\bskip.{0,20}auth', re.IGNORECASE),
        re.compile(r'\breturn\s+(?:true|authenticated|1)\b', re.IGNORECASE),
        re.compile(r'\bbypass.{0,20}token', re.IGNORECASE),
        re.compile(r'\bmissing\s+bearer', re.IGNORECASE),
        re.compile(r'\baccept\s+requests\s+without', re.IGNORECASE),
        re.compile(r'\bno\s+(?:auth|authentication|token)\s+check', re.IGNORECASE),
        re.compile(r'\btreat\s+missing\s+role', re.IGNORECASE),
    ]),
    ('remove_verification', [
        re.compile(r'\bskip\s+verify', re.IGNORECASE),
        re.compile(r'\breturn\s+(?:success|ok|0)\s+when.{0,20}fail', re.IGNORECASE),
        re.compile(r'\bcontinue.{0,20}(?:on\s+error|when\s+nonzero|error)', re.IGNORECASE),
        re.compile(r'\bpass\s+when\s+nonzero', re.IGNORECASE),
        re.compile(r'\bcontinue-on-error', re.IGNORECASE),
        re.compile(r'\bremove.{0,20}verify\s+step', re.IGNORECASE),
        re.compile(r'\bbypass\s+(?:slow\s+)?preflight', re.IGNORECASE),
    ]),
    ('weaken_sandbox', [
        re.compile(r'\bdisable\s+network\s+isolation', re.IGNORECASE),
        re.compile(r'\ballow\s+writes\s+outside\s+workspace', re.IGNORECASE),
        re.compile(r'\bdisable\s+sandbox', re.IGNORECASE),
        re.compile(r'\bself.signed\s+certs\s+for\s+all', re.IGNORECASE),
        re.compile(r'\baccept\s+self.signed', re.IGNORECASE),
    ]),
    ('expose_secrets', [
        re.compile(r'\blog\s+secret', re.IGNORECASE),
        re.compile(r'\bwrite\s+(?:decrypted\s+)?secret\s+to', re.IGNORECASE),
        re.compile(r'\bserialize\s+api\s+token', re.IGNORECASE),
        re.compile(r'\bprint\s+credentials', re.IGNORECASE),
        re.compile(r'\bdebug.{0,20}token\s+value', re.IGNORECASE),
        re.compile(r'\bplain.text.secret', re.IGNORECASE),
    ]),
    ('remove_tests', [
        re.compile(r'\bdelete.{0,20}test', re.IGNORECASE),
        re.compile(r'\bremove.{0,20}test\s+case', re.IGNORECASE),
        re.compile(r'\bdelete\s+fraud\s+regression', re.IGNORECASE),
        re.compile(r'\bdelete.{0,20}(?:auth|authorization)\s+test', re.IGNORECASE),
        re.compile(r'\bremove.{0,20}regression', re.IGNORECASE),
    ]),
]


def score_action_risk(tool, shell_cmd):
    """Score risk based on tool type and shell command."""
    if tool == 'Bash' or tool == 'bash':
        cmd_lower = shell_cmd.lower() if shell_cmd else ''
        for kw in ACTION_RISK_WRITE_KEYWORDS:
            if re.search(r'\b' + kw + r'\b', cmd_lower):
                return 0.9
        if any(re.search(r'\b' + kw + r'\b', cmd_lower) for kw in ['rm -rf', 'dd if=', 'mkfs']):
            return 0.95
        if any(re.search(r'\b' + kw + r'\b', cmd_lower) for kw in ['>', '|', 'tee', 'xargs']):
            return 0.5
        return 0.3
    if tool in ACTION_RISK_EDIT_TOOLS:
        # An edit is not inherently risky — the risk lives in the diff content
        # (secret/verification/antipattern signals), not the act of editing. A
        # 0.4 baseline equalled the warn threshold, so every edit warned and
        # benign edits were indistinguishable from risky ones. Keep it below warn.
        return 0.2
    if tool in ACTION_RISK_READ_TOOLS:
        return 0.1
    return 0.2


def score_diff_risk(old_excerpt, new_excerpt):
    """Score diff content risk; return (score, diff_labels).

    diff_labels may include 'secret_leak', 'verification_bypass', or 'high_diff_risk'.
    """
    if not old_excerpt and not new_excerpt:
        return 0.0, []

    text = (old_excerpt or '') + ' ' + (new_excerpt or '')
    risk = 0.0
    diff_labels = []

    for pat in VERIFICATION_BYPASS_PATTERNS:
        old_match = old_excerpt and pat.search(old_excerpt)
        new_match = new_excerpt and pat.search(new_excerpt)
        if old_match and not new_match:
            risk = max(risk, 0.8)
            if 'verification_bypass' not in diff_labels:
                diff_labels.append('verification_bypass')
            break

    for pat in SECRET_LEAK_PATTERNS:
        if pat.search(text):
            risk = max(risk, 0.9)
            if 'secret_leak' not in diff_labels:
                diff_labels.append('secret_leak')
            break

    if old_excerpt and new_excerpt:
        old_lines = set(old_excerpt.splitlines())
        new_lines = set(new_excerpt.splitlines())
        removed = old_lines - new_lines
        if 'TODO' in removed:
            risk = max(risk, 0.3)
        if '#!/' in old_excerpt and '#!/' not in new_excerpt:
            risk = max(risk, 0.6)
            if (risk >= 0.6 and 'secret_leak' not in diff_labels
                    and 'verification_bypass' not in diff_labels):
                diff_labels.append('high_diff_risk')

    return min(risk, 1.0), diff_labels


def score_antipattern_similarity(old_excerpt, new_excerpt, diff_summary):
    """Score similarity to known anti-pattern categories; return (score, category).

    A category matches when ≥ 2 of its patterns fire on the combined text.
    """
    text = ' '.join(filter(None, [old_excerpt, new_excerpt, diff_summary]))
    if not text.strip():
        return 0.0, ''

    best_score = 0.0
    best_category = ''

    for category, patterns in ANTIPATTERN_CATEGORIES:
        hits = sum(1 for p in patterns if p.search(text))
        if hits >= 2:
            score = min(0.7 + hits * 0.1, 0.95)
        elif hits == 1:
            # These patterns are high-specificity security antipatterns
            # ("missing bearer", "treat missing role as admin", "serialize api
            # token", "remove verify step", ...) — a single confident match is a
            # real risk, not noise. Score it above the warn floor so it surfaces.
            score = 0.6
        else:
            score = 0.0
        if score > best_score:
            best_score = score
            best_category = category

    return best_score, best_category


def score_drift_risk(paths, active_task):
    """Score risk based on task drift (keyword mismatch between paths and task)."""
    if not active_task:
        return 0.0

    path_keywords = set()
    for p in (paths or '').split(','):
        parts = re.findall(r'[a-zA-Z]{4,}', p)
        path_keywords.update(w.lower() for w in parts)

    task_keywords = set(re.findall(r'[a-zA-Z]{4,}', active_task.lower()))

    if not path_keywords or not task_keywords:
        return 0.0

    overlap = path_keywords & task_keywords
    if not overlap:
        return 0.5

    threshold = len(task_keywords) * 0.3
    return 0.1 if len(overlap) >= threshold else 0.5


def compute_overall(action_risk, diff_risk, drift_risk, antipattern_similarity):
    """Compute overall risk as weighted maximum.

    A confirmed antipattern is a direct risk signal, so it is no longer
    discounted (was *0.6, which kept single-match security antipatterns below the
    warn floor). Drift stays a soft signal (*0.5) — on its own a task/path
    keyword mismatch is weak evidence and must not flag benign edits.
    """
    return max(action_risk, diff_risk, drift_risk * 0.5, antipattern_similarity)


def get_recommendation(overall):
    """Map overall risk to policy recommendation."""
    if overall >= 0.90:
        return 'block'
    if overall >= 0.70:
        return 'prompt'
    if overall >= 0.40:
        return 'warn'
    return 'allow'


def build_labels(action_risk, diff_labels, drift_risk, antipattern_category):
    """Build list of matched risk labels."""
    labels = list(diff_labels)  # includes secret_leak, verification_bypass, high_diff_risk
    if action_risk >= 0.7:
        labels.append('high_action_risk')
    elif action_risk >= 0.4:
        labels.append('moderate_action_risk')
    if drift_risk >= 0.4:
        labels.append('task_drift')
    if antipattern_category:
        labels.append(antipattern_category)
    return labels


def build_explanation(tool, action_risk, diff_risk, drift_risk, labels, recommendation):
    """Build human-readable explanation."""
    parts = []
    if 'high_action_risk' in labels:
        parts.append(f"{tool} tool has high action risk ({action_risk:.2f})")
    elif 'moderate_action_risk' in labels:
        parts.append(f"{tool} tool has moderate action risk ({action_risk:.2f})")
    if 'secret_leak' in labels:
        parts.append(f"secret leak pattern detected in diff ({diff_risk:.2f})")
    elif 'verification_bypass' in labels:
        parts.append(f"verification bypass pattern detected in diff ({diff_risk:.2f})")
    elif 'high_diff_risk' in labels:
        parts.append(f"high diff content risk ({diff_risk:.2f})")
    if 'task_drift' in labels:
        parts.append(f"task drift detected ({drift_risk:.2f})")
    if not parts:
        parts.append(f"risk score {action_risk:.2f}")
    parts.append(f"recommendation: {recommendation}")
    return '; '.join(parts)


def score_request(inputs):
    """Score a single request and return output dict."""
    tool = inputs.get('tool', '')
    paths = inputs.get('paths', '')
    shell_cmd = inputs.get('shell_cmd', '')
    old_excerpt = inputs.get('old_excerpt', '')
    new_excerpt = inputs.get('new_excerpt', '')
    active_task = inputs.get('active_task', '')

    action_risk = score_action_risk(tool, shell_cmd)
    diff_risk, diff_labels = score_diff_risk(old_excerpt, new_excerpt)
    drift_risk = score_drift_risk(paths, active_task)
    diff_summary = inputs.get('diff', '')
    antipattern_similarity, antipattern_category = score_antipattern_similarity(
        old_excerpt, new_excerpt, diff_summary)

    overall = compute_overall(action_risk, diff_risk, drift_risk, antipattern_similarity)
    overall = min(overall, 1.0)

    recommendation = get_recommendation(overall)
    labels = build_labels(action_risk, diff_labels, drift_risk, antipattern_category)
    explanation = build_explanation(tool, action_risk, diff_risk, drift_risk, labels, recommendation)

    return {
        'version': 1,
        'role': 'score',
        'outputs': {
            'risk': {
                'overall': round(overall, 3),
                'action_risk': round(action_risk, 3),
                'diff_risk': round(diff_risk, 3),
                'drift_risk': round(drift_risk, 3),
                'antipattern_similarity': round(antipattern_similarity, 3)
            },
            'recommendation': recommendation,
            'labels': labels,
            'antipattern_category': antipattern_category,
        },
        'evidence': {
            'explanation': explanation
        }
    }


def fallback_response():
    """Return safe fallback response on any error."""
    return {
        'version': 1,
        'role': 'score',
        'outputs': {
            'risk': {
                'overall': 0.0,
                'action_risk': 0.0,
                'diff_risk': 0.0,
                'drift_risk': 0.0,
                'antipattern_similarity': 0.0
            },
            'recommendation': 'allow',
            'labels': [],
            'antipattern_category': '',
        },
        'evidence': {
            'explanation': 'sidecar error - degraded to allow'
        }
    }


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        print(json.dumps(fallback_response()), file=sys.stdout)
        return 0

    try:
        if data.get('version') != 1:
            print(json.dumps(fallback_response()), file=sys.stdout)
            return 0

        role = data.get('role', '')
        if role != 'score':
            print(json.dumps(fallback_response()), file=sys.stdout)
            return 0

        inputs = data.get('inputs', {})
        result = score_request(inputs)
        print(json.dumps(result), file=sys.stdout)
        return 0

    except Exception:
        print(json.dumps(fallback_response()), file=sys.stdout)
        return 0


if __name__ == '__main__':
    sys.exit(main())
