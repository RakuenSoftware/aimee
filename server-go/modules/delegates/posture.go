package delegates

import (
	"fmt"
	"strings"
)

// RuntimePostureReport is the daemon-observed state of a running container.
// None of these facts come from inside the untrusted image.
type RuntimePostureReport struct {
	Network           string
	NetworkFailed     bool
	Mounts            string
	MountsFailed      bool
	Environment       string
	EnvironmentFailed bool
}

// RuntimePostureVerdict is the single post-start decision.
type RuntimePostureVerdict struct {
	Refuse bool
	Reason string
}

func expectedMounts(spec SandboxSpec, mountTable string) map[string]struct{} {
	expected := make(map[string]struct{}, len(spec.Mounts))
	for _, mount := range spec.Mounts {
		source := mount.Source
		if mountTable != "" && mount.Kind != SandboxControlSocket {
			source = TranslateMountPath(source, mountTable)
		}
		runtimeWritable := "true"
		if mount.ReadOnly {
			runtimeWritable = "false"
		}
		expected[source+"|"+mount.Target+"|"+runtimeWritable] = struct{}{}
	}
	return expected
}

func parseMountReport(report string) (map[string]struct{}, error) {
	got := make(map[string]struct{})
	for _, raw := range strings.FieldsFunc(report, func(r rune) bool { return r == ';' || r == '\n' }) {
		entry := strings.TrimSpace(raw)
		if entry == "" {
			continue
		}
		source, rest, found := strings.Cut(entry, "|")
		target, writable, foundMode := strings.Cut(rest, "|")
		if !found || !foundMode || strings.Contains(writable, "|") ||
			(writable != "true" && writable != "false") {
			return nil, fmt.Errorf("malformed mount report entry %q", entry)
		}
		if _, ok := cleanAbs(source); !ok {
			return nil, fmt.Errorf("invalid mount source in runtime report: %q", source)
		}
		if _, ok := cleanAbs(target); !ok {
			return nil, fmt.Errorf("invalid mount target in runtime report: %q", target)
		}
		key := source + "|" + target + "|" + writable
		if _, duplicate := got[key]; duplicate {
			return nil, fmt.Errorf("duplicate runtime mount: %s", key)
		}
		got[key] = struct{}{}
	}
	return got, nil
}

func verifyMountReport(spec SandboxSpec, mountTable, report string) error {
	expected := expectedMounts(spec, mountTable)
	got, err := parseMountReport(report)
	if err != nil {
		return err
	}
	if len(got) != len(expected) {
		return fmt.Errorf("runtime mount set differs: got %d mounts, want %d", len(got), len(expected))
	}
	for mount := range expected {
		if _, ok := got[mount]; !ok {
			return fmt.Errorf("runtime is missing required mount %s", mount)
		}
	}
	return nil
}

func parseEnvironmentReport(report string) (map[string]string, error) {
	got := make(map[string]string)
	for _, raw := range strings.Split(report, "\n") {
		entry := strings.TrimSpace(strings.TrimSuffix(raw, "\r"))
		if entry == "" {
			continue
		}
		name, value, found := strings.Cut(entry, "=")
		if !found || name == "" || strings.ContainsAny(name, " \t\r\n") {
			return nil, fmt.Errorf("malformed environment report entry %q", entry)
		}
		if _, duplicate := got[name]; duplicate {
			return nil, fmt.Errorf("duplicate runtime environment name %s", name)
		}
		if looksLikeCredential(name) {
			return nil, fmt.Errorf("credential-looking environment reached the sandbox: %s", name)
		}
		got[name] = value
	}
	return got, nil
}

func verifyEnvironmentReport(spec SandboxSpec, report string) error {
	got, err := parseEnvironmentReport(report)
	if err != nil {
		return err
	}
	for _, expected := range spec.Env {
		if value, ok := got[expected.Name]; !ok || value != expected.Value {
			return fmt.Errorf("runtime environment does not contain exact %s value", expected.Name)
		}
	}
	return nil
}

// VerifyRuntimePosture fails closed unless the daemon proves no network,
// exactly the requested mounts, and a credentialless environment containing
// the module's exact required control endpoint.
func VerifyRuntimePosture(spec SandboxSpec, mountTable string, report RuntimePostureReport) RuntimePostureVerdict {
	if err := ValidateSandboxSpec(spec); err != nil {
		return RuntimePostureVerdict{Refuse: true, Reason: "invalid sandbox specification: " + err.Error()}
	}
	if verdict := JudgeIsolation(ParseIsolationProbe(report.Network, report.NetworkFailed)); verdict.Refuse {
		return RuntimePostureVerdict{Refuse: true, Reason: verdict.Reason}
	}
	if report.MountsFailed {
		return RuntimePostureVerdict{Refuse: true, Reason: "could not verify runtime mounts"}
	}
	if err := verifyMountReport(spec, mountTable, report.Mounts); err != nil {
		return RuntimePostureVerdict{Refuse: true, Reason: err.Error()}
	}
	if report.EnvironmentFailed {
		return RuntimePostureVerdict{Refuse: true, Reason: "could not verify runtime environment"}
	}
	if err := verifyEnvironmentReport(spec, report.Environment); err != nil {
		return RuntimePostureVerdict{Refuse: true, Reason: err.Error()}
	}
	return RuntimePostureVerdict{}
}
