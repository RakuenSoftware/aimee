package memory

import "testing"

// Port the retired C fixture's fairness/bounds requirements to the production
// Go RRF implementation. The ranking policy is explicitly RRF, not interleaving.
func TestGoCandidateFusionFairnessBoundsAndDuplicateVotes(t *testing.T) {
	lexical, semantic := []Record{}, []Record{}
	for i := 0; i < 32; i++ {
		lexical = append(lexical, Record{ID: int64(100 + i)})
	}
	for i := 0; i < 4; i++ {
		semantic = append(semantic, Record{ID: int64(200 + i)})
	}
	rows := fusePersonal(lexical, semantic, 8)
	if len(rows) != 8 {
		t.Fatal(rows)
	}
	seen := map[int64]bool{}
	secondary := 0
	for _, row := range rows {
		if seen[row.ID] {
			t.Fatal("duplicate candidate")
		}
		seen[row.ID] = true
		if row.ID >= 200 {
			secondary++
		}
	}
	if secondary != 4 {
		t.Fatal("long lexical arm starved semantic arm", rows)
	}
	one := Record{ID: 1, Content: "first"}
	copy := Record{ID: 1, Content: "duplicate"}
	rows = fusePersonal([]Record{one, copy, copy}, []Record{{ID: 2}}, 2)
	if rows[0].ID != 1 || rows[0].Content != "first" || rows[0].retrievalScore != rows[1].retrievalScore {
		t.Fatal("duplicate votes or metadata replacement", rows)
	}
	if len(fusePersonal(nil, nil, 4)) != 0 || len(fusePersonal(lexical, semantic, 0)) != 0 {
		t.Fatal("empty/zero capacity")
	}
}
