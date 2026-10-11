package memory

import "testing"

func TestSpatialFactsRetainDistanceEndpointsAndCompoundPlace(t *testing.T) {
	actor := FactActor{Principal: "connector", Role: "user", Rank: 30, Authenticated: 1}
	text := "But the car wash is 200 meters from <@333333333333333333> 's house, and is located in Kansas City, Kansas."
	facts := measurementFactCandidates(text, "now", 1, 2, actor)
	if len(facts) != 2 || facts[0].Subject != "Distance between <@333333333333333333>'s house and car wash" || facts[0].Relation != "has_distance" || facts[0].Object != "200 meters" || facts[1].Subject != "car wash" || facts[1].Relation != "located_in" || facts[1].Object != "Kansas City, Kansas" || facts[0].Actor != actor {
		t.Fatalf("%+v", facts)
	}
	reversed := spatialFactCandidates("Alex's house is 0.2 km from the car wash.", "now", 1, 2, modelFactActor())
	forward := spatialFactCandidates("The car wash is 0.2 kilometers from Alex's house.", "now", 1, 2, modelFactActor())
	if len(forward) != 1 || len(reversed) != 1 || factIdentityComponent(forward[0].Subject) != factIdentityComponent(reversed[0].Subject) || forward[0].Object != reversed[0].Object || forward[0].Actor.Rank != 10 {
		t.Fatalf("%+v %+v", forward, reversed)
	}
}
func TestSpatialFactsAbstainOnUnspecifiedReferencesQuestionsAndInferences(t *testing.T) {
	for _, text := range []string{"The car wash is 200 meters away.", "How far is the car wash from the Himalayas?", "Is the car wash located in Kansas?", "The car wash is 200 meters from Alex's house?", "The car wash is not located in Kansas.", "Someone said the car wash is located in Kansas.", "I think the car wash is located in Kansas.", "The car wash is 200 meters from my house."} {
		if facts := spatialFactCandidates(text, "now", 1, 2, modelFactActor()); len(facts) != 0 {
			t.Fatalf("%q: %+v", text, facts)
		}
	}
	facts := spatialFactCandidates("The car wash is located in Kansas City, Kansas.", "now", 1, 2, modelFactActor())
	if len(facts) != 1 || facts[0].Subject != "car wash" || facts[0].Object != "Kansas City, Kansas" {
		t.Fatalf("%+v", facts)
	}
}
