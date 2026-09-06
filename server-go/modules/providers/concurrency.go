package providers

import configclient "github.com/JBailes/aimee/server-go/config"

// The roster is authoritative. Pending config projections share its atomic
// commit and are replayed after a crash before serving the next request.
func queueConcurrency(root, before object) {
	pending := rows(root, "concurrency_pending")
	for _, model := range rows(root, "models") {
		old := find(rows(before, "models"), str(model, "name"))
		if str(model, "model") != "" && (old == nil || str(old, "model") != str(model, "model") || number(old, "max_parallel") != number(model, "max_parallel")) {
			limit := number(model, "max_parallel")
			if limit <= 0 {
				limit = 3
			}
			pending = append(pending, object{"model": str(model, "model"), "limit": limit})
		}
	}
	for _, old := range rows(before, "models") {
		id := str(old, "model")
		used := false
		for _, model := range rows(root, "models") {
			used = used || str(model, "model") == id
		}
		if id != "" && !used {
			pending = append(pending, object{"model": id})
		}
	}
	if len(pending) > 0 {
		root["concurrency_pending"] = pending
	}
}
func (m *Manager) recoverConfig() error {
	if m.config == nil {
		return nil
	}
	dirty := false
	_, err := m.store.transaction(false, func(root object) (object, error) { dirty = len(rows(root, "concurrency_pending")) > 0; return nil, nil })
	if err != nil || !dirty {
		return err
	}
	_, err = m.store.transaction(true, func(root object) (object, error) {
		for _, item := range rows(root, "concurrency_pending") {
			id, limit := str(item, "model"), int(number(item, "limit"))
			if limit > 0 {
				err = m.config.SetModelConcurrency(configclient.ModelConcurrencyMutation{Model: id, Limit: limit})
			} else {
				err = m.config.RemoveModelConcurrency(id)
			}
			if err != nil {
				return nil, err
			}
		}
		delete(root, "concurrency_pending")
		return nil, nil
	})
	return err
}
