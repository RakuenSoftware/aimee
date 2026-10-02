#!/usr/bin/env python3
"""Datalog sidecar for graph reasoning over KB_STORE artifacts.

Implements semi-naive bottom-up evaluation of stratified Datalog rules
over a read-only fact snapshot supplied by the C caller.

Protocol (stdin):
{
  "version": 1,
  "role": "reason",
  "model_version": "datalog-semi-naive-v1",
  "prompt_version": "ruleset-v1",
  "scope": {"kind": "...", "id": "..."},
  "inputs": {
    "query": "contradiction_ok(?a, ?b)",
    "bindings": {"?a": "artifact-id-1"},
    "facts": [
      ["artifact", "id1", "synthesis", "committed", "user", "proj1", "1.0",
       "2026-01-01", "", "2026-12-31"],
      ["artifact_link", "id1", "id2", "contradicts"],
      ["entity_edge", "EntityA", "alias", "EntityB", "1", "", ""]
    ],
    "ruleset": "v1",
    "limit": 256,
    "row_budget": 10000,
    "time_limit_ms": 5000
  }
}

Protocol (stdout on success):
{
  "version": 1,
  "status": "ok",
  "bindings": [
    {
      "vars": {"?a": "id1", "?b": "id2"},
      "citations": ["artifact_link:id1:id2:contradicts", "artifact:id1"]
    }
  ],
  "stats": {"facts_evaluated": 42, "derived_facts": 5, "elapsed_ms": 12}
}

Protocol (stdout on error):
{"version": 1, "status": "error", "error": "..."}

See docs/proposals/accepted/graph-reasoning-case-based-recall-and-contradiction-logic.md
"""

from __future__ import annotations

import json
import sys
import time
from collections import defaultdict
from typing import Any, Dict, FrozenSet, List, Optional, Set, Tuple

# ---------------------------------------------------------------------------
# Term / variable helpers
# ---------------------------------------------------------------------------

Binding = Dict[str, str]
Fact = Tuple[str, ...]   # (pred_name, arg0, arg1, ...)


def is_var(term: str) -> bool:
    return term.startswith("?")


def apply_binding(terms: List[str], binding: Binding) -> List[str]:
    return [binding.get(t, t) if is_var(t) else t for t in terms]


def is_ground(terms: List[str]) -> bool:
    return all(not is_var(t) for t in terms)


def unify(pattern: List[str], fact: Tuple[str, ...], binding: Binding) -> Optional[Binding]:
    """Return extended binding if pattern matches fact, else None."""
    if len(pattern) != len(fact):
        return None
    result = dict(binding)
    for p, f in zip(pattern, fact):
        if is_var(p):
            if p in result:
                if result[p] != f:
                    return None
            else:
                result[p] = f
        else:
            if p != f:
                return None
    return result


# ---------------------------------------------------------------------------
# Rule representation
# ---------------------------------------------------------------------------

class Atom:
    __slots__ = ("pred", "args")

    def __init__(self, pred: str, args: List[str]) -> None:
        self.pred = pred
        self.args = args

    def __repr__(self) -> str:
        return f"{self.pred}({', '.join(self.args)})"


class Rule:
    __slots__ = ("head", "body")

    def __init__(self, head: Atom, body: List[Atom]) -> None:
        self.head = head
        self.body = body


# ---------------------------------------------------------------------------
# Ruleset v1 — eight rule families
# ---------------------------------------------------------------------------

# Rules are written as (head_pred, head_args, [(body_pred, body_args), ...])
# Variables start with "?".

_RULESET_V1: List[Tuple[str, List[str], List[Tuple[str, List[str]]]]] = [
    # ---- Entity family ----
    # alias_of(?x, ?y) :- entity_edge(?x, "alias", ?y, ?, ?, ?).
    ("alias_of", ["?x", "?y"], [
        ("entity_edge", ["?x", "alias", "?y", "?w", "?vf", "?vu"])]),

    # same_entity(?x, ?y, ?e) :- alias_of(?x, ?e), alias_of(?y, ?e).
    ("same_entity", ["?x", "?y", "?e"], [
        ("alias_of", ["?x", "?e"]),
        ("alias_of", ["?y", "?e"])]),

    # same_entity(?x, ?y, ?x) :- alias_of(?y, ?x).
    ("same_entity", ["?x", "?y", "?x"], [
        ("alias_of", ["?y", "?x"])]),

    # ---- Claim family ----
    # superseded_by(?a, ?b) :- artifact_link(?a, ?b, "supersedes").
    ("superseded_by", ["?a", "?b"], [
        ("artifact_link", ["?a", "?b", "supersedes"])]),

    # citation_reachable(?a, ?b) :- artifact_link(?a, ?b, ?k).
    # (any link makes b reachable from a)
    ("citation_reachable", ["?a", "?b"], [
        ("artifact_link", ["?a", "?b", "?k"])]),

    # citation_reachable(?a, ?c) :- citation_reachable(?a, ?b), citation_reachable(?b, ?c).
    ("citation_reachable", ["?a", "?c"], [
        ("citation_reachable", ["?a", "?b"]),
        ("citation_reachable", ["?b", "?c"])]),

    # ---- Code family ----
    # implements(?claim, ?code) :- artifact_link(?claim, ?code, "implements").
    ("implements", ["?claim", "?code"], [
        ("artifact_link", ["?claim", "?code", "implements"])]),

    # implements_transitively(?claim, ?code) :- implements(?claim, ?code).
    ("implements_transitively", ["?claim", "?code"], [
        ("implements", ["?claim", "?code"])]),

    # implements_transitively(?claim, ?code) :-
    #     implements_transitively(?claim, ?mid),
    #     implements_transitively(?mid, ?code).
    ("implements_transitively", ["?claim", "?code"], [
        ("implements_transitively", ["?claim", "?mid"]),
        ("implements_transitively", ["?mid", "?code"])]),

    # ---- Workflow family ----
    # workflow_corrects(?w, ?a) :- artifact_link(?w, ?a, "corrects").
    ("workflow_corrects", ["?w", "?a"], [
        ("artifact_link", ["?w", "?a", "corrects"])]),

    # ---- Review family ----
    # rejected_artifact(?a) :- artifact(?a, ?, "rejected", ?, ?, ?, ?, ?, ?).
    ("rejected_artifact", ["?a"], [
        ("artifact", ["?a", "?k", "rejected", "?sk", "?si", "?conf", "?cat",
                      "?vf", "?vu"])]),

    # ---- Temporal family ----
    # has_validity(?a, ?vf, ?vu) :- artifact(?a, ?, ?, ?, ?, ?, ?, ?vf, ?vu).
    ("has_validity", ["?a", "?vf", "?vu"], [
        ("artifact", ["?a", "?k", "?s", "?sk", "?si", "?conf", "?cat", "?vf", "?vu"])]),

    # ---- Provenance family ----
    # supports(?a, ?b) :- artifact_link(?a, ?b, "supports").
    ("supports", ["?a", "?b"], [
        ("artifact_link", ["?a", "?b", "supports"])]),

    # ---- Contradiction family ----
    # contradicts_link(?a, ?b) :- artifact_link(?a, ?b, "contradicts").
    ("contradicts_link", ["?a", "?b"], [
        ("artifact_link", ["?a", "?b", "contradicts"])]),

    # contradicts_link(?a, ?b) :- artifact_link(?b, ?a, "contradicts").
    ("contradicts_link", ["?a", "?b"], [
        ("artifact_link", ["?b", "?a", "contradicts"])]),

    # contradiction_ok(?a, ?b) :-
    #     contradicts_link(?a, ?b),
    #     artifact(?a, ?, "committed", ?, ?, ?, ?, ?, ?),
    #     artifact(?b, ?, "committed", ?, ?, ?, ?, ?, ?).
    ("contradiction_ok", ["?a", "?b"], [
        ("contradicts_link", ["?a", "?b"]),
        ("artifact", ["?a", "?ka", "committed", "?ska", "?sia", "?ca", "?cata", "?vfa", "?vua"]),
        ("artifact", ["?b", "?kb", "committed", "?skb", "?sib", "?cb", "?catb", "?vfb", "?vub"])]),
]


def build_rules() -> List[Rule]:
    rules: List[Rule] = []
    for head_pred, head_args, body_specs in _RULESET_V1:
        head = Atom(head_pred, head_args)
        body = [Atom(bp, list(ba)) for bp, ba in body_specs]
        rules.append(Rule(head, body))
    return rules


# ---------------------------------------------------------------------------
# Semi-naive bottom-up evaluator
# ---------------------------------------------------------------------------

class DatalogEngine:
    def __init__(
        self,
        rules: List[Rule],
        edb: Dict[str, Set[Tuple[str, ...]]],
        row_budget: int,
        time_limit_ms: int,
    ) -> None:
        self.rules = rules
        self.edb = edb   # extensional DB: pred -> set of ground tuples
        self.idb: Dict[str, Set[Tuple[str, ...]]] = defaultdict(set)
        self.row_budget = row_budget
        self.time_limit_ms = time_limit_ms
        self.facts_evaluated = 0
        # citation map: derived fact -> set of supporting EDB fact strings
        self.citations: Dict[Tuple[str, Tuple[str, ...]], Set[str]] = {}

    def _lookup(self, pred: str) -> Set[Tuple[str, ...]]:
        s = self.edb.get(pred, set())
        return s | self.idb.get(pred, set())

    def _eval_body(
        self, body: List[Atom], binding: Binding, cite: List[str]
    ) -> List[Tuple[Binding, List[str]]]:
        """Return all extended bindings satisfying the body atoms."""
        if not body:
            return [(binding, cite)]
        atom = body[0]
        rest = body[1:]
        results: List[Tuple[Binding, List[str]]] = []
        pat = apply_binding(atom.args, binding)
        for fact in self._lookup(atom.pred):
            self.facts_evaluated += 1
            ext = unify(pat, fact, binding)
            if ext is not None:
                cite_key = f"{atom.pred}:{':'.join(fact)}"
                extended = self._eval_body(rest, ext, cite + [cite_key])
                results.extend(extended)
        return results

    def evaluate(self) -> None:
        deadline_ns = time.perf_counter_ns() + self.time_limit_ms * 1_000_000
        total_derived = 0
        changed = True
        while changed:
            if time.perf_counter_ns() > deadline_ns:
                break
            if total_derived >= self.row_budget:
                break
            changed = False
            for rule in self.rules:
                solutions = self._eval_body(rule.body, {}, [])
                for binding, cite in solutions:
                    ground_args = apply_binding(rule.head.args, binding)
                    if not is_ground(ground_args):
                        continue
                    tup = tuple(ground_args)
                    if tup not in self.idb[rule.head.pred]:
                        self.idb[rule.head.pred].add(tup)
                        fact_key = (rule.head.pred, tup)
                        if fact_key not in self.citations:
                            self.citations[fact_key] = set(cite)
                        changed = True
                        total_derived += 1
                        if total_derived >= self.row_budget:
                            break
                if total_derived >= self.row_budget:
                    break

    def query(
        self, query_atom: Atom, initial_binding: Binding, limit: int
    ) -> List[Dict[str, Any]]:
        """Return solution bindings for query_atom."""
        results: List[Dict[str, Any]] = []
        pat = apply_binding(query_atom.args, initial_binding)
        for fact in self._lookup(query_atom.pred):
            if len(results) >= limit:
                break
            ext = unify(pat, fact, initial_binding)
            if ext is None:
                continue
            # Only return vars that appear in query_atom
            exposed: Dict[str, str] = {}
            for arg in query_atom.args:
                if is_var(arg) and arg in ext:
                    exposed[arg] = ext[arg]
            # Gather citations
            cite_set: Set[str] = set()
            fact_key = (query_atom.pred, fact)
            if fact_key in self.citations:
                cite_set = self.citations[fact_key]
            results.append({"vars": exposed, "citations": sorted(cite_set)})
        return results


# ---------------------------------------------------------------------------
# Fact loader
# ---------------------------------------------------------------------------

def load_edb(raw_facts: List[List[str]]) -> Dict[str, Set[Tuple[str, ...]]]:
    edb: Dict[str, Set[Tuple[str, ...]]] = defaultdict(set)
    for row in raw_facts:
        if not row or not isinstance(row, list):
            continue
        pred = str(row[0])
        args = tuple(str(a) for a in row[1:])
        edb[pred].add(args)
    return edb


# ---------------------------------------------------------------------------
# Query atom parser
# ---------------------------------------------------------------------------

def parse_atom(s: str) -> Atom:
    """Parse 'pred(?x, ?y)' or 'pred(?x)' into an Atom."""
    s = s.strip()
    paren = s.index("(")
    pred = s[:paren].strip()
    args_str = s[paren + 1:s.rindex(")")].strip()
    if not args_str:
        return Atom(pred, [])
    args = [a.strip() for a in args_str.split(",")]
    return Atom(pred, args)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    raw = sys.stdin.read()
    start_ns = time.perf_counter_ns()

    try:
        req = json.loads(raw)
    except Exception as e:
        json.dump({"version": 1, "status": "error", "error": f"JSON parse: {e}"}, sys.stdout)
        sys.stdout.write("\n")
        return

    try:
        inputs = req.get("inputs", {})
        query_str: str = inputs.get("query", "")
        initial_bindings: Dict[str, str] = inputs.get("bindings", {})
        raw_facts: List[List[str]] = inputs.get("facts", [])
        limit: int = min(int(inputs.get("limit", 256)), 256)
        row_budget: int = min(int(inputs.get("row_budget", 10000)), 50000)
        time_limit_ms: int = min(int(inputs.get("time_limit_ms", 5000)), 30000)

        if not query_str:
            raise ValueError("inputs.query is required")

        query_atom = parse_atom(query_str)
        edb = load_edb(raw_facts)
        rules = build_rules()

        engine = DatalogEngine(rules, edb, row_budget, time_limit_ms)
        engine.evaluate()

        bindings = engine.query(query_atom, initial_bindings, limit)
        elapsed_ms = (time.perf_counter_ns() - start_ns) // 1_000_000

        json.dump(
            {
                "version": 1,
                "status": "ok",
                "bindings": bindings,
                "stats": {
                    "facts_evaluated": engine.facts_evaluated,
                    "derived_facts": sum(len(v) for v in engine.idb.values()),
                    "elapsed_ms": int(elapsed_ms),
                },
            },
            sys.stdout,
        )
        sys.stdout.write("\n")

    except Exception as e:
        json.dump({"version": 1, "status": "error", "error": str(e)}, sys.stdout)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
