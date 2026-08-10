import { describe, it, expect } from "vitest";
import { groupByProvider } from "./Providers";

/* The runtime records are flat (one per endpoint+model); the page's job is to
 * present them the way an operator thinks about them. These pin the two
 * decisions in that mapping that are not obvious. */
const target = (over: Record<string, unknown>) =>
  ({ name: "n", endpoint: "e", model: "m", provider: "p", enabled: true, ...over }) as never;

describe("groupByProvider", () => {
  it("nests every model of one provider under a single entry", () => {
    const groups = groupByProvider([
      target({ name: "a:sonnet", provider: "anthropic", endpoint: "https://api.anthropic.com/v1", model: "claude-sonnet-5" }),
      target({ name: "a:opus", provider: "anthropic", endpoint: "https://api.anthropic.com/v1", model: "claude-opus-5" }),
    ]);
    expect(groups).toHaveLength(1);
    expect(groups[0].provider).toBe("anthropic");
    /* Sorted, so the list does not reshuffle between refreshes. */
    expect(groups[0].models.map((m) => m.model)).toEqual(["claude-opus-5", "claude-sonnet-5"]);
  });

  it("keeps one vendor configured twice apart, because credentials differ", () => {
    /* A direct account and a gateway that resells the same vendor are two
     * providers. Collapsing them on the vendor name alone would file one
     * provider's models under the other's endpoint and key -- and "add a model
     * here" would then silently use the wrong credentials. */
    const groups = groupByProvider([
      target({ name: "direct", provider: "anthropic", endpoint: "https://api.anthropic.com/v1", model: "claude-sonnet-5" }),
      target({ name: "gw", provider: "anthropic", endpoint: "https://gateway.internal/v1", model: "claude-sonnet-5" }),
    ]);
    expect(groups).toHaveLength(2);
    expect(new Set(groups.map((g) => g.endpoint))).toEqual(
      new Set(["https://api.anthropic.com/v1", "https://gateway.internal/v1"]),
    );
  });

  it("keeps a CLI seat with no endpoint rather than dropping it", () => {
    /* The claude seat has an empty endpoint and an empty model; it is still a
     * configured thing an operator needs to see. */
    const groups = groupByProvider([target({ name: "claude", provider: "claude", endpoint: "", model: "" })]);
    expect(groups).toHaveLength(1);
    expect(groups[0].models).toHaveLength(1);
  });

  it("returns nothing for an empty roster instead of throwing", () => {
    expect(groupByProvider([])).toEqual([]);
  });
});
