package executionpolicy

import (
	"bytes"
	"encoding/json"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// CurrentActionCompositionPolicy shares the existing operator policy loader.
// Callers cannot supply a more permissive policy inside action JSON.
func CurrentActionCompositionPolicy() (ActionCompositionPolicy, error) {
	policy, err := defaultPolicyLoader()
	if err != nil {
		return ActionCompositionPolicy{}, err
	}
	if policy == nil {
		return ActionCompositionPolicy{}, nil
	}
	return policy.Actions, nil
}

// ActionPolicyGeneration binds admission to the same policy document used by
// ordinary tool authorization, including its composition limits.
func ActionPolicyGeneration() (string, error) {
	policy, err := defaultPolicyLoader()
	if err != nil {
		return "", err
	}
	return actionDigest(policy), nil
}

// IssueActionIntent fills owner identities and a stable expiry after trusted
// resource resolution. Reissuing the same key cannot refresh changed material
// or turn an expired/unknown effect into a fresh dispatch permission.
func IssueActionIntent(principal, root string, state []byte, intent ActionIntent, now time.Time) (ActionIntent, error) {
	generation, err := ActionPolicyGeneration()
	if err != nil {
		return ActionIntent{}, err
	}
	intent.SchemaVersion = 1
	intent.Principal = principal
	intent.Root = root
	intent.IdempotencyKey = actionDigest([]string{principal, root, intent.Request, intent.Attempt})
	intent.ID = intent.IdempotencyKey
	intent.PolicyGeneration = generation
	intent.Expires = now.Add(15 * time.Minute)
	if len(state) > 0 {
		var journal ActionJournal
		if json.Unmarshal(state, &journal) != nil || journal.validate() != nil {
			return ActionIntent{}, errors.New("invalid action journal")
		}
		if prior, ok := journal.Actions[intent.IdempotencyKey]; ok {
			intent.Expires = prior.Intent.Expires
		}
	}
	return intent, nil
}

// BindActionFreshness adds the owner commitment after the host has acquired
// current authority and source observations. It does not upgrade an expired
// observation or grant authorization itself.
func BindActionFreshness(intent ActionIntent, f ActionFreshness) ActionFreshness {
	f.IntentDigest = actionDigest(intent)
	return f
}

// AuthorizeActionAdmission rereads one operator snapshot at final admission and
// checks the exact tool-owner payload. Raw arguments are not returned or stored.
func AuthorizeActionAdmission(intent ActionIntent, arguments json.RawMessage) (ActionCompositionPolicy, error) {
	policy, err := defaultPolicyLoader()
	if err != nil {
		return ActionCompositionPolicy{}, err
	}
	if actionDigest(policy) != intent.PolicyGeneration {
		return ActionCompositionPolicy{}, errors.New("action policy changed")
	}
	var args map[string]any
	decoder := json.NewDecoder(bytes.NewReader(arguments))
	decoder.UseNumber()
	if decodeSingle(decoder, &args) != nil || args == nil || actionDigest(args) != intent.PayloadDigest {
		return ActionCompositionPolicy{}, errors.New("effective action payload changed")
	}
	path, ok := args["path"].(string)
	if !ok || !filepath.IsAbs(path) || filepath.Clean(path) != path || intent.Destination != "file:"+path {
		return ActionCompositionPolicy{}, errors.New("exact action destination changed")
	}
	write := false
	switch intent.Tool {
	case "write_file", "edit_file", "edit_symbol":
		write = true
	case "read_file", "read_symbol", "grep":
	default:
		return ActionCompositionPolicy{}, errors.New("action resource adapter unavailable")
	}
	class, e := fileActionClass(policy, write, path)
	canonical, _ := json.Marshal(args)
	if e != nil || class != intent.Class || intent.WorkUnits != strconv.Itoa(len(canonical)) {
		return ActionCompositionPolicy{}, errors.New("trusted action resource classification changed")
	}
	effect := "filesystem"
	if intent.Class == "external_publish" {
		effect = "external-tool"
	}
	if !evaluateBaseline(request{Tool: intent.Tool, SideEffect: effect, Arguments: arguments}, policy).Allowed {
		return ActionCompositionPolicy{}, errors.New("current operator policy refuses action")
	}
	if policy == nil {
		return ActionCompositionPolicy{}, nil
	}
	return policy.Actions, nil
}

// FileActionClass classifies operator-declared sensitive and publication paths
// after tool-owner canonicalization. A model side_effect label is never read.
func FileActionClass(write bool, path string) (string, error) {
	policy, err := defaultPolicyLoader()
	if err != nil {
		return "", err
	}
	return fileActionClass(policy, write, path)
}

func fileActionClass(policy *operatorPolicy, write bool, path string) (string, error) {
	class := "read_only"
	if write {
		class = "file_write"
	}
	if policy == nil {
		return class, nil
	}
	prefixes := policy.Actions.SensitivePathPrefixes
	if write {
		prefixes = policy.Actions.PublishedPathPrefixes
	}
	for _, prefix := range prefixes {
		if !filepath.IsAbs(prefix) || filepath.Clean(prefix) != prefix {
			return "", errors.New("invalid action path policy")
		}
		if path == prefix || strings.HasPrefix(path, strings.TrimSuffix(prefix, "/")+"/") {
			if write {
				return "external_publish", nil
			}
			return "sensitive_read", nil
		}
	}
	return class, nil
}

func AuthorizeActionObservation(path string) error {
	policy, err := defaultPolicyLoader()
	if err != nil {
		return err
	}
	arguments, _ := json.Marshal(map[string]string{"path": path})
	if !evaluateBaseline(request{Tool: "read_file", SideEffect: "filesystem", Arguments: arguments}, policy).Allowed {
		return errors.New("current policy refuses object observation")
	}
	return nil
}
