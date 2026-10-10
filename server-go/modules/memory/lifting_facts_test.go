package memory

import "testing"

func TestLiftingFactsGroundNamedMeasurementsAndSourceActor(t *testing.T) {
	actor := FactActor{Principal: "connector", Role: "user", Rank: 30, Authenticated: 1}
	for _, text := range []string{"Kibukx can lift 500 pounds.", "I'm telling you, Kibukx can lift 500 pounds.", "But Kibukx can lift 500 lbs!"} {
		facts := measurementFactCandidates(text, "now", 1, 2, actor)
		if len(facts) != 1 || facts[0].Subject != "Kibukx" || facts[0].Relation != "can_lift" || facts[0].Object != "500 pounds" || facts[0].Actor != actor || !factFunctional("can_lift") {
			t.Fatalf("%q: %+v", text, facts)
		}
	}
	facts := patternFactCandidates("Kibukx is 6 feet tall; the Kibukx mountains are 69 feet tall; Kibukx can lift 226.8 kg.", "now", 1, 2, modelFactActor())
	if len(facts) != 3 || facts[2].Object != "226.8 kilograms" || facts[2].Actor.Rank != 10 || facts[2].Evidence.SourceSpan == facts[0].Evidence.SourceSpan {
		t.Fatalf("%+v", facts)
	}
}

func TestLiftingFactsAbstainOnQuestionsNegationAndReportedClaims(t *testing.T) {
	for _, text := range []string{"How much can Kibukx lift?", "Can Kibukx lift 500 pounds?", "Kibukx can lift 500 pounds?", "Kibukx cannot lift 500 pounds", "Kibukx can not lift 500 pounds", "He can lift 500 pounds", "My brother can lift 500 pounds", "Someone said Kibukx can lift 500 pounds", "I think Kibukx can lift 500 pounds", `"Kibukx can lift 500 pounds"`} {
		if facts := measurementFactCandidates(text, "now", 1, 2, modelFactActor()); len(facts) != 0 {
			t.Fatalf("%q: %+v", text, facts)
		}
	}
}
