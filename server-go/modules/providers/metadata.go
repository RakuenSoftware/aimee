package providers

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

type Metadata struct {
	mu                       sync.Mutex
	home                     string
	loaded                   bool
	entries                  []object
	aliases, detect, windows [][]string
}

func newMetadata(home string) *Metadata {
	m := &Metadata{home: home}
	var seed map[string][][]string
	_ = json.Unmarshal([]byte(metadataSeed), &seed)
	m.aliases, m.detect, m.windows = seed["aliases"], seed["detect"], seed["windows"]
	return m
}
func (m *Metadata) detectProvider(id string) string {
	id = strings.ToLower(id)
	if id == "" {
		return ""
	}
	for _, row := range m.detect {
		if strings.HasPrefix(id, row[0]) {
			return row[1]
		}
	}
	for _, row := range m.detect {
		if len(row[0]) >= 4 && strings.Contains(id, row[0]) {
			return row[1]
		}
	}
	return "openai"
}
func (m *Metadata) alias(ref string) object {
	for _, row := range m.aliases {
		if strings.EqualFold(ref, row[0]) {
			return object{"provider": row[1], "model": row[2]}
		}
	}
	return nil
}
func metadataKey(p, id string) string { return strings.ToLower(p + ":" + id) }
func (m *Metadata) load() error {
	if m.loaded {
		return nil
	}
	cache := os.Getenv("XDG_CACHE_HOME")
	if cache == "" {
		home, _ := os.UserHomeDir()
		cache = filepath.Join(home, ".cache")
	}
	paths := []string{filepath.Join(cache, "aimee/models_dev.json"), os.Getenv("AIMEE_MODELS_DEV_SNAPSHOT"), "/usr/local/share/aimee/models_dev_snapshot.json", "data/models_dev_snapshot.json", "../data/models_dev_snapshot.json", "../../../../data/models_dev_snapshot.json"}
	var entries []object
	seen := map[string]bool{}
	snapshotLoaded := false
	for pi, path := range paths {
		if path == "" || pi > 0 && snapshotLoaded {
			continue
		}
		data, err := os.ReadFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		parsed, err := parseMetadata(data)
		if err != nil {
			return fmt.Errorf("invalid model metadata in %s: %w", path, err)
		}
		if len(parsed) > 0 {
			for _, entry := range parsed {
				key := metadataKey(str(entry, "provider"), str(entry, "model"))
				if !seen[key] {
					entries = append(entries, entry)
					seen[key] = true
				}
			}
			if pi > 0 {
				snapshotLoaded = true
			}
		}
	}
	for _, entry := range fallbackCapabilities {
		key := metadataKey(str(entry, "provider"), str(entry, "model"))
		if !seen[key] {
			v := copyObject(entry)
			finishCapability(v)
			entries = append(entries, v)
			seen[key] = true
		}
	}
	for _, entry := range entries {
		if metadataKey(str(entry, "provider"), str(entry, "model")) == "openai:gpt-4-turbo" {
			entry["deprecated"] = true
		}
	}
	byKey := map[string]int{}
	for i, row := range entries {
		byKey[metadataKey(str(row, "provider"), str(row, "model"))] = i
	}
	overrides := os.Getenv("AIMEE_MODEL_CAPABILITY_OVERRIDES")
	if overrides == "" {
		overrides = filepath.Join(m.home, "model_capability_overrides.json")
	}
	for _, path := range []string{filepath.Join(m.home, "model_overrides.json"), overrides} {
		data, err := os.ReadFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		parsed, err := parseMetadata(data)
		if err != nil {
			return err
		}
		for _, row := range parsed {
			row["metadata_source"] = "override"
			k := metadataKey(str(row, "provider"), str(row, "model"))
			if i, ok := byKey[k]; ok {
				entries[i] = row
			} else {
				byKey[k] = len(entries)
				entries = append(entries, row)
			}
		}
	}
	m.entries, m.loaded = entries, true
	return nil
}
func parseMetadata(data []byte) ([]object, error) {
	if len(data) > 32<<20 {
		return nil, errors.New("metadata exceeds 32 MiB")
	}
	var raw any
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, err
	}
	out := []object{}
	if root, ok := raw.(map[string]any); ok {
		if flat, ok := root["models"].([]any); ok {
			raw = flat
		} else {
			providers := make([]string, 0, len(root))
			for key := range root {
				providers = append(providers, key)
			}
			sort.Strings(providers)
			for _, provider := range providers {
				if vendor, id, ok := strings.Cut(provider, "/"); ok {
					if entry, ok := root[provider].(map[string]any); ok {
						v := object{"provider": vendor, "model": id}
						usable := false
						for input, output := range map[string]string{"contextWindow": "context_window", "maxTokens": "max_output", "inputCost": "cost_in_per_mtok", "outputCost": "cost_out_per_mtok", "cacheReadCost": "cost_cache_read_per_mtok"} {
							if value, exists := entry[input]; exists && number(entry, input) >= 0 {
								v[output] = value
								usable = usable || number(entry, input) > 0
							}
						}
						flags := 0
						for field, bit := range map[string]int{"tools": 2, "vision": 4, "pdf": 8} {
							if boolean(entry, field, false) {
								flags |= bit
							}
						}
						if usable || flags != 0 {
							v["flags_mask"] = flags
							v["deprecated"] = boolean(entry, "deprecated", false)
							finishCapability(v)
							out = append(out, v)
						}
					}
					continue
				}
				vendor, ok := root[provider].(map[string]any)
				if !ok {
					continue
				}
				models, ok := vendor["models"].(map[string]any)
				if !ok {
					continue
				}
				ids := make([]string, 0, len(models))
				for id := range models {
					ids = append(ids, id)
				}
				sort.Strings(ids)
				for _, id := range ids {
					entry, ok := models[id].(map[string]any)
					if !ok {
						continue
					}
					v := object{"provider": provider, "model": id, "display_name": str(entry, "name"), "open_weights": boolean(entry, "open_weights", false), "deprecated": boolean(entry, "deprecated", false), "knowledge_cutoff": str(entry, "knowledge")}
					if limits, ok := entry["limit"].(map[string]any); ok {
						v["context_window"], v["max_output"] = number(limits, "context"), number(limits, "output")
					}
					if cost, ok := entry["cost"].(map[string]any); ok {
						for input, output := range map[string]string{"input": "cost_in_per_mtok", "output": "cost_out_per_mtok", "cache_read": "cost_cache_read_per_mtok"} {
							if n, exists := cost[input]; exists {
								v[output] = n
							}
						}
						bands := map[int]object{}
						for _, tier := range rows(cost, "tiers") {
							spec, _ := tier["tier"].(map[string]any)
							size := int(number(spec, "size"))
							if str(spec, "type") != "context" || size <= 0 || size >= 2147483647 {
								continue
							}
							if _, ok := tier["input"]; !ok {
								continue
							}
							if _, ok := tier["output"]; !ok {
								continue
							}
							bands[size] = object{"above_tokens": size, "in_per_mtok": number(tier, "input"), "out_per_mtok": number(tier, "output"), "cache_read_per_mtok": number(tier, "cache_read")}
						}
						sizes := []int{}
						for size := range bands {
							sizes = append(sizes, size)
						}
						sort.Ints(sizes)
						outBands := []object{}
						for _, size := range sizes {
							outBands = append(outBands, bands[size])
						}
						v["price_bands"] = outBands
					}
					flags := 32
					if boolean(entry, "reasoning", false) {
						flags |= 1
					}
					if boolean(entry, "tool_call", false) {
						flags |= 2
					}
					if modalities, ok := entry["modalities"].(map[string]any); ok {
						if input, ok := modalities["input"].([]any); ok {
							for _, mod := range input {
								switch mod {
								case "image":
									flags |= 4
								case "pdf":
									flags |= 8
								case "audio":
									flags |= 16
								}
							}
						}
					}
					v["flags_mask"] = flags
					finishCapability(v)
					out = append(out, v)
				}
			}
			return out, nil
		}
	}
	flat, ok := raw.([]any)
	if !ok {
		return nil, errors.New("metadata must be a models array or provider catalog")
	}
	for _, item := range flat {
		v, ok := item.(map[string]any)
		if !ok {
			return nil, errors.New("invalid metadata record")
		}
		if str(v, "model") == "" {
			v["model"] = str(v, "model_id")
		}
		if str(v, "provider") == "" {
			provider, id, ok := strings.Cut(str(v, "id"), "/")
			if ok {
				v["provider"], v["model"] = provider, id
			}
		}
		if str(v, "provider") == "" || str(v, "model") == "" {
			return nil, errors.New("metadata provider and model required")
		}
		if _, ok := v["flags_mask"]; !ok {
			v["flags_mask"] = number(v, "flags")
		}
		for _, field := range []string{"open_weights", "deprecated"} {
			if _, ok := v[field].(bool); !ok {
				v[field] = number(v, field) != 0
			}
		}
		finishCapability(v)
		out = append(out, v)
	}
	return out, nil
}
func flag(name string) int {
	switch strings.ToLower(name) {
	case "reasoning":
		return 1
	case "tools", "tool":
		return 2
	case "vision", "image":
		return 4
	case "pdf":
		return 8
	case "audio":
		return 16
	case "streaming", "stream":
		return 32
	case "thinking_adaptive":
		return 64
	}
	return 0
}
func finishCapability(v object) {
	flags := int(number(v, "flags_mask"))
	names := []string{}
	for _, name := range []string{"reasoning", "tools", "vision", "pdf", "audio", "streaming", "thinking_adaptive"} {
		if flags&flag(name) != 0 {
			names = append(names, name)
		}
	}
	v["flags"], v["capabilities"] = strings.Join(names, ","), strings.Join(names, ",")
	modalities := "text"
	if flags&4 != 0 {
		modalities += ",image"
	}
	if flags&16 != 0 {
		modalities += ",audio"
	}
	v["modalities"] = modalities
}
func (m *Metadata) contextWindow(id string) int {
	for _, entry := range m.entries {
		if strings.EqualFold(str(entry, "model"), id) && number(entry, "context_window") > 0 {
			return int(number(entry, "context_window"))
		}
	}
	for _, row := range m.windows {
		if strings.HasPrefix(strings.ToLower(id), strings.ToLower(row[0])) {
			n, _ := strconv.Atoi(row[1])
			return n
		}
	}
	return 0
}
func (m *Metadata) lookup(provider, id string) object {
	if id == "" {
		return nil
	}
	if provider == "" {
		if alias := m.alias(id); alias != nil {
			provider, id = str(alias, "provider"), str(alias, "model")
		} else {
			provider = m.detectProvider(id)
		}
	}
	for _, entry := range m.entries {
		if strings.EqualFold(str(entry, "provider"), provider) && strings.EqualFold(str(entry, "model"), id) {
			return copyObject(entry)
		}
	}
	for _, entry := range fallbackCapabilities {
		if metadataKey(str(entry, "provider"), str(entry, "model")) == metadataKey(provider, id) {
			out := copyObject(entry)
			finishCapability(out)
			return out
		}
	}
	semantic := id
	if i := strings.LastIndex(id, "/"); i >= 0 {
		semantic = id[i+1:]
	}
	semantic = strings.ToLower(semantic)
	effective := provider
	if effective == "openrouter" {
		effective = m.detectProvider(semantic)
	}
	flags := 0
	switch effective {
	case "anthropic":
		flags = 34
		if strings.HasPrefix(semantic, "claude-3") || strings.HasPrefix(semantic, "claude-opus-4") || strings.HasPrefix(semantic, "claude-sonnet-4") || strings.HasPrefix(semantic, "claude-haiku-4") {
			flags |= 12
		}
		if strings.Contains(semantic, "opus") || strings.Contains(semantic, "sonnet") {
			flags |= 1
		}
	case "openai":
		flags = 34
		if strings.HasPrefix(semantic, "gpt-4o") || strings.HasPrefix(semantic, "gpt-5") {
			flags |= 4
		}
		if strings.HasPrefix(semantic, "gpt-5") || strings.HasPrefix(semantic, "o1") || strings.HasPrefix(semantic, "o3") {
			flags |= 1
		}
	case "gemini", "google":
		flags = 46
		if strings.HasPrefix(semantic, "gemini-2.5") {
			flags |= 1
		}
	case "mistral":
		flags = 34
	case "minimax":
		flags = 35
	case "moonshotai", "moonshot":
		flags = 34
		if strings.HasPrefix(semantic, "kimi-k2") || strings.HasPrefix(semantic, "kimi-k3") {
			flags |= 1
		}
	}
	window := m.contextWindow(id)
	if window == 0 {
		window = m.contextWindow(semantic)
	}
	output := 8192
	if flags&1 != 0 {
		output = 32768
	}
	if window > 0 && output > window {
		output = window
	}
	v := object{"provider": provider, "model": id, "context_window": window, "max_output": output, "flags_mask": flags, "deprecated": strings.Contains(semantic, "deprecated") || metadataKey(provider, id) == "openai:gpt-4-turbo", "open_weights": false}
	finishCapability(v)
	return v
}
func (m *Metadata) request(operation string, a object) (object, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if operation == "metadata.refresh" {
		m.loaded = false
	}
	if err := m.load(); err != nil {
		return nil, err
	}
	switch operation {
	case "metadata.published", "metadata.override":
		key := metadataKey(str(a, "provider"), str(a, "model"))
		var found object
		for _, entry := range m.entries {
			if metadataKey(str(entry, "provider"), str(entry, "model")) == key && (operation != "metadata.override" || str(entry, "metadata_source") == "override") {
				found = copyObject(entry)
				break
			}
		}
		return object{"status": "ok", "model": found}, nil
	case "metadata.context_window":
		id := str(a, "model")
		for _, row := range m.entries {
			if strings.EqualFold(str(row, "model"), id) && number(row, "context_window") > 0 {
				return object{"model": object{"context_window": row["context_window"]}}, nil
			}
		}
		return object{"model": object{"context_window": m.contextWindow(id)}}, nil
	case "metadata.refresh":
		return object{"status": "ok", "count": len(m.entries), "message": "model metadata refreshed"}, nil
	case "metadata.detect":
		return object{"status": "ok", "provider": m.detectProvider(str(a, "model"))}, nil
	case "metadata.alias":
		v := m.alias(str(a, "name"))
		return object{"status": "ok", "model": v}, nil
	case "metadata.aliases":
		out := []object{}
		for _, row := range m.aliases {
			out = append(out, object{"provider": row[1], "model": row[2]})
		}
		return object{"status": "ok", "models": out, "count": len(out)}, nil
	case "metadata.list":
		required := flag(str(a, "capability"))
		if str(a, "capability") != "" && required == 0 {
			return nil, errors.New("unknown model capability")
		}
		required |= int(number(a, "required_flags"))
		out := []object{}
		limit := int(number(a, "limit"))
		if limit <= 0 {
			limit = 256
		}
		total := 0
		for _, row := range m.entries {
			if int(number(row, "flags_mask"))&required != required || boolean(a, "open_weights_only", false) && !boolean(row, "open_weights", false) {
				continue
			}
			total++
			if len(out) < limit {
				out = append(out, copyObject(row))
			}
		}
		return object{"status": "ok", "models": out, "count": total}, nil
	default:
		provider, id := str(a, "provider"), str(a, "model")
		if ref := str(a, "name"); ref != "" {
			if alias := m.alias(ref); alias != nil {
				provider, id = str(alias, "provider"), str(alias, "model")
			} else if p, v, ok := strings.Cut(ref, ":"); ok {
				provider, id = p, v
			} else {
				id = ref
			}
		}
		if id == "" {
			if operation == "metadata.max_output" {
				return object{"status": "ok", "model": object{"max_output": 8192}}, nil
			}
			return nil, errors.New("model required")
		}
		return object{"status": "ok", "model": m.lookup(provider, id)}, nil
	}
}

func (m *Manager) modelView(model object) object {
	out := modelView(model)
	reply, err := m.metadata.request("metadata.show", object{"provider": str(out, "catalog_provider"), "model": str(model, "model")})
	if err != nil {
		return out
	}
	cap, _ := reply["model"].(map[string]any)
	if cap == nil {
		return out
	}
	if label := str(cap, "display_name"); label != "" {
		out["model_display_name"] = label
	}
	for _, field := range []string{"context_window", "max_output"} {
		if number(model, field) <= 0 && number(cap, field) > 0 {
			out["effective_"+field] = number(cap, field)
			out[field+"_source"] = "resolved"
		}
	}
	for axis, source := range map[string]string{"in": "cost_in_per_mtok", "out": "cost_out_per_mtok", "cached": "cost_cache_read_per_mtok"} {
		if _, declared := model["price_"+axis+"_per_mtok"]; !declared {
			if price, ok := cap[source]; ok {
				out["price_base_"+axis+"_per_mtok"] = price
			}
		}
	}
	bands := rows(cap, "price_bands")
	for _, band := range bands {
		for axis, field := range map[string]string{"in": "in_per_mtok", "out": "out_per_mtok", "cached": "cache_read_per_mtok"} {
			if price, declared := model["price_"+axis+"_per_mtok"]; declared {
				band[field] = price
			}
		}
		band["cached_per_mtok"] = band["cache_read_per_mtok"]
	}
	if len(bands) > 0 {
		out["price_bands"] = bands
	}
	return out
}
