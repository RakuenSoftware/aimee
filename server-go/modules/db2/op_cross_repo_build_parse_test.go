package db2

import (
	"strings"
	"testing"
)

func TestCMakeBuildDepsIgnoresWhatIsNotADependency(t *testing.T) {
	// The comment on cmakeBuildDeps claims comments and string literals are
	// skipped as they are met, "which is what stops a commented-out
	// FetchContent block from becoming a dependency". That is the whole value of
	// the function: an edge from a disabled block is not an error anywhere, it
	// is a dependency graph with a relationship nobody has.
	for _, c := range []struct {
		name    string
		content string
		want    []string
	}{
		{
			name: "a real FetchContent block",
			content: `FetchContent_Declare(dep
			  GIT_REPOSITORY https://example.invalid/org/dep.git
			  GIT_TAG v1)`,
			want: []string{"https://example.invalid/org/dep.git"},
		},
		{
			name:    "a line comment",
			content: "# GIT_REPOSITORY https://example.invalid/org/ghost.git",
			want:    nil,
		},
		{
			name: "a bracket comment",
			content: `#[[
			  GIT_REPOSITORY https://example.invalid/org/ghost.git
			]]`,
			want: nil,
		},
		{
			name:    "a quoted url is still a url",
			content: `GIT_REPOSITORY "https://example.invalid/org/quoted.git"`,
			want:    []string{"https://example.invalid/org/quoted.git"},
		},
		{
			name:    "the same dependency twice is one edge",
			content: "GIT_REPOSITORY https://e.invalid/a.git\nGIT_REPOSITORY https://e.invalid/a.git",
			want:    []string{"https://e.invalid/a.git"},
		},
	} {
		t.Run(c.name, func(t *testing.T) {
			got := cmakeBuildDeps(c.content)
			if len(got) != len(c.want) {
				t.Fatalf("got %d deps %+v, want %d", len(got), got, len(c.want))
			}
			for index, want := range c.want {
				if got[index].Ref != want {
					t.Errorf("dep %d = %q, want %q", index, got[index].Ref, want)
				}
			}
		})
	}
}

func TestAVariableURLIsRecordedAtLowConfidence(t *testing.T) {
	// The comment says a URL built from a variable "is recorded at low
	// confidence: the reference is real but its value is not known until CMake
	// runs". Recording it as certain would put a literal ${MIRROR} into the
	// dependency table as though it were a host.
	got := cmakeBuildDeps("GIT_REPOSITORY ${MIRROR}/org/dep.git")
	if len(got) != 1 {
		t.Fatalf("got %d deps, want 1", len(got))
	}
	if !got[0].LowConf {
		t.Error("a URL containing a variable was recorded as certain")
	}
	if plain := cmakeBuildDeps("GIT_REPOSITORY https://e.invalid/a.git"); len(plain) != 1 ||
		plain[0].LowConf {
		t.Error("a literal URL was recorded as uncertain")
	}
}

func TestGitmodulesTakesURLsAndSkipsComments(t *testing.T) {
	got := gitmodulesBuildDeps(`[submodule "vendor/a"]
	path = vendor/a
	url = https://example.invalid/org/a.git
# url = https://example.invalid/org/commented.git
; url = https://example.invalid/org/semicolon.git
	URL = https://example.invalid/org/upper.git`)
	if len(got) != 2 {
		t.Fatalf("got %d deps %+v, want 2", len(got), got)
	}
	// Case-insensitive, because .gitmodules is read by git and git does not care.
	if got[1].Ref != "https://example.invalid/org/upper.git" {
		t.Errorf("dep 1 = %q; an uppercase key was missed", got[1].Ref)
	}
	for _, dep := range got {
		if strings.Contains(dep.Ref, "commented") || strings.Contains(dep.Ref, "semicolon") {
			t.Errorf("a commented-out submodule became a dependency: %q", dep.Ref)
		}
	}
}

func TestCargoTakesGitAndPathButNotWordsContainingThem(t *testing.T) {
	// path dependencies are the point: "a dependency on a sibling checkout,
	// which is exactly the cross-repository edge this table is for".
	got := cargoBuildDeps(`[dependencies]
a = { git = "https://example.invalid/org/a.git" }
b = { path = "../b" }
c = { registry = "crates-io" } # path = "../ghost"
digit = "1.0"`)
	refs := map[string]bool{}
	for _, dep := range got {
		refs[dep.Ref] = true
	}
	if !refs["https://example.invalid/org/a.git"] || !refs["../b"] {
		t.Fatalf("git and path were not both taken: %+v", got)
	}
	if refs["../ghost"] {
		t.Error("a dependency inside a comment was taken")
	}
	// "digit" contains "git", and a key is only a key when the character before
	// it is not part of a longer word.
	for ref := range refs {
		if ref == "1.0" {
			t.Error(`"digit" was read as the key "git"`)
		}
	}
}

func TestExtractBuildDepsDispatchesOnTheFileName(t *testing.T) {
	// The dispatch is by base name, so a path prefix must not change the answer
	// and an unrelated file must produce nothing rather than being guessed at.
	cmake := `GIT_REPOSITORY https://example.invalid/org/a.git`
	for _, path := range []string{"CMakeLists.txt", "deep/nested/CMakeLists.txt", "x/dep.cmake"} {
		if got := extractBuildDeps(path, cmake); len(got) != 1 {
			t.Errorf("%s produced %d deps, want 1", path, len(got))
		}
	}
	if got := extractBuildDeps("README.md", cmake); got != nil {
		t.Errorf("a file nothing parses produced %+v", got)
	}
}

func TestBuildRefRepoTakesTheRepositoryAndDropsCredentials(t *testing.T) {
	// The comment: "Credentials in a URL are dropped before the host, which also
	// keeps a token out of the evidence the table would otherwise hold." That is
	// a secret-handling claim, and it was asserted nowhere.
	for _, c := range []struct{ ref, want string }{
		{"https://example.invalid/org/Repo.git", "repo"},
		{"https://user:token@example.invalid/org/repo.git", "repo"},
		{"git@example.invalid:org/repo.git", "repo"},
		{"../sibling", "sibling"},
		{"https://example.invalid/org/repo/", "repo"},
		{"", ""},
	} {
		if got := buildRefRepo(c.ref); got != c.want {
			t.Errorf("buildRefRepo(%q) = %q, want %q", c.ref, got, c.want)
		}
	}

	// A token must not survive into the recorded name, which is the half the
	// comment is really about.
	if got := buildRefRepo("https://user:supersecret@example.invalid/org/repo.git"); strings.Contains(got, "supersecret") {
		t.Errorf("a credential survived into %q", got)
	}

	// THE CASE THAT ACTUALLY EXERCISES THE CREDENTIAL STRIP, found by deleting
	// that clause and watching every case above still pass. With a slash after
	// the '@', the last-component rule drops the credential on its own and the
	// strip is doing nothing observable. Without one, the strip is the only
	// thing between a token and the recorded name.
	//
	// Worth the paragraph because the first version of this test asserted the
	// comment's claim and passed for a different reason -- a test passing for a
	// reason it does not state, inside a test written to check for that.
	if got := buildRefRepo("user:supersecret@repo.git"); got != "repo" {
		t.Errorf("buildRefRepo(%q) = %q, want %q -- without a slash after the "+
			"credential nothing else removes it",
			"user:supersecret@repo.git", got, "repo")
	}
}

func TestBuildDepsAreCappedAndDeduplicated(t *testing.T) {
	found := []buildDepRef{}
	for index := 0; index < crossRepoMaxDepsPerFile+10; index++ {
		found = appendBuildDep(found, "https://example.invalid/org/"+
			strings.Repeat("a", index+1)+".git", "fetch", false)
	}
	if len(found) != crossRepoMaxDepsPerFile {
		t.Fatalf("kept %d deps, want the cap of %d", len(found), crossRepoMaxDepsPerFile)
	}
	// The cap is a refusal to grow, not a refusal to answer: what was collected
	// before it is still there.
	if found[0].Ref == "" {
		t.Error("the cap discarded what had already been collected")
	}
}
