#!/usr/bin/env python3
"""Cost/token/time savings report for the 3-measurement supervised benchmark.

Three spend levels, all for the SAME expensive primary model:

  1. Default (no aimee)      -- the primary solves the task with its full raw
                                context. Derived from the ledger as
                                realized + avoided: the economizer records the
                                context it folded away as usage_kind='avoided',
                                so realized+avoided is what the primary WOULD
                                have spent with no economizer.
  2. Aimee economized        -- the same primary run through aimee's ingress:
     (no delegates)             only the realized tokens are actually billed;
                                the avoided context never hits the model.
  3. Aimee + delegates       -- the primary only supervises a free/local fleet
                                (tools-off best-of-N selection); its realized
                                tokens are just the small supervision prompt.

The economizer folds INPUT context (avoided completion tokens are ~0), so input
and output are priced separately at frontier-equivalent rates.

This module is pure: it takes already-summed ledger token counts (see
bench_cost_savings.py for the fleet-ledger attribution that produces them) and
turns them into the postable table. That keeps every number reproducible and
unit-testable without a live fleet.
"""
from __future__ import annotations
from dataclasses import dataclass, asdict

# Frontier-equivalent price for the primary model, USD per 1,000,000 tokens.
# The fleet prices local models at $0, so to tell a cost story we price the
# primary at published frontier rates. These are a documented ASSUMPTION (a
# GPT-5-class tier); override with --price-in / --price-out on the driver.
DEFAULT_PRICE = {"input_per_mtok": 1.25, "output_per_mtok": 10.00}


@dataclass(frozen=True)
class ArmTokens:
    """Summed primary-model tokens for one arm, split by economizer disposition.
    `realized_*` were actually billed; `avoided_*` were folded away by the
    economizer (counterfactual: what a no-economizer run would have added)."""
    realized_prompt: int = 0
    realized_completion: int = 0
    avoided_prompt: int = 0
    avoided_completion: int = 0

    @property
    def realized_total(self) -> int:
        return self.realized_prompt + self.realized_completion

    @property
    def default_prompt(self) -> int:
        return self.realized_prompt + self.avoided_prompt

    @property
    def default_completion(self) -> int:
        return self.realized_completion + self.avoided_completion

    @property
    def default_total(self) -> int:
        return self.default_prompt + self.default_completion


def _usd(prompt: int, completion: int, price: dict) -> float:
    return round(
        prompt / 1_000_000 * price["input_per_mtok"]
        + completion / 1_000_000 * price["output_per_mtok"],
        4,
    )


def summarize(primary: ArmTokens, supervised: ArmTokens,
              default_wall_s: float, delegate_wall_s: float,
              price: dict | None = None) -> dict:
    """Build the 5-column report from the primary-alone arm (measurements 1 & 2)
    and the supervised arm (measurement 3).

    - Default          = primary.default_*      (realized + avoided)
    - Aimee economized = primary.realized_*      (economizer on, no delegates)
    - Aimee+delegates  = supervised.realized_*   (supervisor tokens only)
    - Delegate saved   = economized - (aimee+delegates), i.e. what delegating the
                         solve on top of the economizer removes from the primary.
    """
    price = price or DEFAULT_PRICE

    default_tok = primary.default_total
    default_usd = _usd(primary.default_prompt, primary.default_completion, price)

    econ_tok = primary.realized_total
    econ_usd = _usd(primary.realized_prompt, primary.realized_completion, price)

    deleg_tok = supervised.realized_total
    deleg_usd = _usd(supervised.realized_prompt, supervised.realized_completion, price)

    # Savings deltas.
    econ_saved_tok = default_tok - econ_tok            # economizer alone
    econ_saved_usd = round(default_usd - econ_usd, 4)
    deleg_saved_tok = econ_tok - deleg_tok             # delegates on top of economizer
    deleg_saved_usd = round(econ_usd - deleg_usd, 4)
    total_saved_tok = default_tok - deleg_tok          # both, vs default
    total_saved_usd = round(default_usd - deleg_usd, 4)

    def pct(saved: int, base: int) -> float:
        return round(100.0 * saved / base, 1) if base else 0.0

    return {
        "price_per_mtok": price,
        "levels": {
            "default":          {"tokens": default_tok, "input": primary.default_prompt,
                                 "output": primary.default_completion, "usd": default_usd},
            "aimee_economized": {"tokens": econ_tok, "input": primary.realized_prompt,
                                 "output": primary.realized_completion, "usd": econ_usd},
            "aimee_delegates":  {"tokens": deleg_tok, "input": supervised.realized_prompt,
                                 "output": supervised.realized_completion, "usd": deleg_usd},
        },
        "savings": {
            "economizer":  {"tokens": econ_saved_tok, "usd": econ_saved_usd,
                            "pct_of_default": pct(econ_saved_tok, default_tok)},
            "delegates":   {"tokens": deleg_saved_tok, "usd": deleg_saved_usd,
                            "pct_of_economized": pct(deleg_saved_tok, econ_tok)},
            "total":       {"tokens": total_saved_tok, "usd": total_saved_usd,
                            "pct_of_default": pct(total_saved_tok, default_tok)},
        },
        "wall_seconds": {"default": round(default_wall_s, 1),
                         "aimee_delegates": round(delegate_wall_s, 1)},
    }


def _fmt_tok(n: int) -> str:
    return f"{n:,}"


def render_markdown(summary: dict) -> str:
    lv = summary["levels"]
    sv = summary["savings"]
    w = summary["wall_seconds"]
    p = summary["price_per_mtok"]
    lines = [
        f"_Primary priced at frontier-equivalent ${p['input_per_mtok']}/1M input, "
        f"${p['output_per_mtok']}/1M output._",
        "",
        "| Measurement | Tokens | Cost $ |",
        "|---|---:|---:|",
        f"| Default (no aimee) | {_fmt_tok(lv['default']['tokens'])} | ${lv['default']['usd']:.2f} |",
        f"| Aimee economized (no delegates) | {_fmt_tok(lv['aimee_economized']['tokens'])} | ${lv['aimee_economized']['usd']:.2f} |",
        f"| Aimee + delegates | {_fmt_tok(lv['aimee_delegates']['tokens'])} | ${lv['aimee_delegates']['usd']:.2f} |",
        "",
        "| Saving | Tokens | Cost $ | Reduction |",
        "|---|---:|---:|---:|",
        f"| Economizer (vs default) | {_fmt_tok(sv['economizer']['tokens'])} | ${sv['economizer']['usd']:.2f} | {sv['economizer']['pct_of_default']}% |",
        f"| Delegates (vs economized) | {_fmt_tok(sv['delegates']['tokens'])} | ${sv['delegates']['usd']:.2f} | {sv['delegates']['pct_of_economized']}% |",
        f"| **Total (vs default)** | **{_fmt_tok(sv['total']['tokens'])}** | **${sv['total']['usd']:.2f}** | **{sv['total']['pct_of_default']}%** |",
        "",
        "| Time | Seconds |",
        "|---|---:|",
        f"| Default | {w['default']} |",
        f"| Aimee with delegates | {w['aimee_delegates']} |",
    ]
    return "\n".join(lines)


def render_headline(summary: dict) -> str:
    """The single-line, postable summary."""
    lv, sv, w = summary["levels"], summary["savings"], summary["wall_seconds"]
    return (
        f"Default {_fmt_tok(lv['default']['tokens'])} tok (${lv['default']['usd']:.2f}) "
        f"-> economized {_fmt_tok(lv['aimee_economized']['tokens'])} tok "
        f"(${lv['aimee_economized']['usd']:.2f}) "
        f"-> +delegates {_fmt_tok(lv['aimee_delegates']['tokens'])} tok "
        f"(${lv['aimee_delegates']['usd']:.2f}) | "
        f"total -{sv['total']['pct_of_default']}% tokens, "
        f"-${sv['total']['usd']:.2f} | "
        f"time {w['default']}s -> {w['aimee_delegates']}s"
    )
