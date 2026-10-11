package memory

import "testing"

func TestDiscordAliasesRequireExplicitCompleteIdentityStatement(t *testing.T) {
	actor := FactActor{Principal: "connector", Role: "user", Rank: 30, Authenticated: 1}
	source := "<@!333333333333333333> is also known as Kibukx."
	got := measurementFactCandidates(source, "now", 1, 2, actor)
	if len(got) != 1 || got[0].Subject != "<@333333333333333333>" || got[0].Object != "Kibukx" || got[0].Relation != "also_known_as" || got[0].Actor != actor {
		t.Fatalf("%+v", got)
	}
	for _, text := range []string{"Is <@333333333333333333> also known as Kibukx?", "Someone said <@333333333333333333> is also known as Kibukx", "<@333333333333333333> is not also known as Kibukx", "<@333333333333333333> is also known as Kibukx?", "<@123> is also known as Kibukx"} {
		if got := discordAliasFactCandidates(text, "now", 1, 2, actor); len(got) != 0 {
			t.Fatalf("%q: %+v", text, got)
		}
	}
}
