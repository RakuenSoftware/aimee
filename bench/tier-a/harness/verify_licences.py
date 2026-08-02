"""Re-read each candidate's licence from the Hugging Face model card.

models.json records a licence per candidate, and the whole recommendation turns
on whether a model is MIT/Apache-2.0 or not. That field is worth checking against
the source rather than trusting a note I wrote by hand.

Exits non-zero if the hub disagrees with models.json.
"""

import json
import pathlib
import sys

from huggingface_hub import HfApi

HERE = pathlib.Path(__file__).parent
PERMISSIVE = {"apache-2.0", "mit"}


def main():
    reg = json.loads((HERE / "models.json").read_text())
    api = HfApi()
    mismatches = []
    print(f"{'model':42s} {'declared':16s} {'hub':16s} permissive")
    for m in reg["models"]:
        try:
            info = api.model_info(m["id"])
            cd = info.card_data or {}
            hub = cd.get("license") or "?"
            # A custom licence is declared as license: other plus a license_name.
            # Comparing against the bare "other" would flag every such model.
            if hub == "other" and cd.get("license_name"):
                hub = cd["license_name"]
        except Exception as e:  # gated repo, network, renamed model
            hub = f"ERR:{type(e).__name__}"
        declared = m["licence"]
        ok = hub.lower().replace(" ", "") == declared.lower().replace(" ", "")
        if not ok and not hub.startswith("ERR"):
            mismatches.append((m["id"], declared, hub))
        print(f"{m['id']:42s} {declared:16s} {hub:16s} "
              f"{'yes' if hub.lower() in PERMISSIVE else 'no'}")

    if mismatches:
        print("\nMISMATCH — models.json disagrees with the hub:", file=sys.stderr)
        for mid, d, h in mismatches:
            print(f"  {mid}: declared {d}, hub says {h}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
