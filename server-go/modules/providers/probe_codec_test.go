package providers

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

var codecFixtureDirectory string

func TestMain(m *testing.M) {
	status := m.Run()
	if codecFixtureDirectory != "" {
		entries, _ := os.ReadDir(codecFixtureDirectory)
		sanitizerFailed := false
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), "sanitizer-") {
				sanitizerFailed = true
				raw, _ := os.ReadFile(filepath.Join(codecFixtureDirectory, entry.Name()))
				fmt.Fprintf(os.Stderr, "shared C codec sanitizer failure: %s\n%s\n", entry.Name(), raw)
			}
		}
		if sanitizerFailed {
			status = 1
		} else {
			os.RemoveAll(codecFixtureDirectory)
		}
	}

	os.Exit(status)
}

// Lazy compilation uses the actual shared C codec. Compile-only package checks
// do not execute this, and no provider transport/configuration is involved.
func init() {
	var once sync.Once
	var binary string
	modelCodecExecutable = func() string {
		once.Do(func() {
			root, err := filepath.Abs("../../..")
			if err != nil {
				panic(err)
			}
			directory, err := os.MkdirTemp("", "aimee-model-codec-")
			if err != nil {
				panic(err)
			}
			codecFixtureDirectory = directory
			binary = filepath.Join(directory, "codec")
			src := filepath.Join(root, "src")
			main := filepath.Join(directory, "main.c")
			mainSource := "#include <aimee/translation/codec_stdio.h>\nint main(void){return aimee_model_codec_stdio(stdin,stdout);}\n"
			if os.Getenv("AIMEE_CODEC_TEST_SAN") == "1" {
				mainSource += fmt.Sprintf("const char *__asan_default_options(void){return %q;}\nconst char *__ubsan_default_options(void){return %q;}\n", "detect_leaks=1:log_path="+filepath.Join(directory, "sanitizer-address"), "halt_on_error=1:log_path="+filepath.Join(directory, "sanitizer-undefined"))
			}
			if err = os.WriteFile(main, []byte(mainSource), 0600); err != nil {
				panic(err)
			}

			args := []string{"-std=c11", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-I" + src, "-I" + filepath.Join(src, "vendor/headers"), "-I" + filepath.Join(src, "headers"), "-I" + filepath.Join(src, "core/codec/include"), "-I" + filepath.Join(src, "modules/ir/include"), "-I" + filepath.Join(src, "modules/translation/include"), main}
			includes, _ := filepath.Glob(filepath.Join(src, "modules", "*", "include"))
			privateIncludes, _ := filepath.Glob(filepath.Join(src, "modules", "*"))
			for _, directory := range privateIncludes {
				if info, err := os.Stat(directory); err == nil && info.IsDir() {
					includes = append(includes, directory)
				}
			}
			for _, directory := range append(includes, filepath.Join(src, "db1_client"), filepath.Join(src, "modules/kb/c"), filepath.Join(src, "modules/config"), filepath.Join(src, "core/types/include")) {
				args = append(args, "-I"+directory)
			}
			for _, file := range []string{"modules/translation/codec_stdio.c", "modules/translation/wire_codec.c", "modules/translation/aimee_backend_openai.c", "modules/translation/aimee_backend_anthropic.c", "modules/translation/aimee_backend_responses.c", "modules/translation/aimee_frontend_openai.c", "modules/translation/aimee_frontend_anthropic.c", "modules/translation/aimee_frontend_responses.c", "modules/translation/aimee_ir_stream.c", "modules/ir/aimee_ir.c", "modules/ir/session_event.c", "modules/ir/request_codec.c", "modules/ir/response_codec.c", "vendor/cJSON.c", "aimee_sha256.c", "text.c", "util.c"} {
				args = append(args, filepath.Join(src, file))
			}
			if os.Getenv("AIMEE_CODEC_TEST_SAN") == "1" {
				args = append(args, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie")
			}
			args = append(args, "-lm", "-lcrypto", "-pthread", "-o", binary)
			if output, err := exec.Command("cc", args...).CombinedOutput(); err != nil {
				panic(string(output) + err.Error())
			}
		})
		return binary
	}
}

func TestProbeMissingSharedCodecCannotGrantReadiness(t *testing.T) {
	previous := modelCodecExecutable
	modelCodecExecutable = func() string { return filepath.Join(t.TempDir(), "missing-codec") }
	defer func() { modelCodecExecutable = previous }()
	if _, err := probeWire(probeRequest("fixture", "openai_chat"), false); err == nil {
		t.Fatal("missing shared codec rendered a request")
	}
	response, err := probeHTTPResponse([]byte(`{"choices":[{"finish_reason":"stop","message":{"content":"ok"}}]}`), "openai_chat")
	if err == nil || response.CompleteText() {
		t.Fatal("missing shared codec granted readiness")
	}
	if len(response.Raw) == 0 {
		t.Fatal("failed diagnostic lost original frame")
	}
}
