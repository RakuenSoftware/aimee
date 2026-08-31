package economizer

import (
	"fmt"
	"strings"
)

// Deterministic command-aware tool-output condensation primitives.
//
// Ported from src/modules/economizer/tool_condense.c.
//
// Pure and deterministic, no LLM. These are the toolbox the per-command family
// rules compose from. The contract throughout is FAIL-OPEN: when a primitive
// cannot improve on its input it returns the input unchanged, because losing
// tool output is worse than failing to shrink it.

// TCCeiling is the last-resort capture ceiling (2 MB) for tool output when the
// command-filter lever is on: the seam captures up to this so the FULL output
// reaches the lever instead of being truncated at the old 32 KB read cap.
const TCCeiling = 2 * 1024 * 1024

// The C file uses TWO different line models, and conflating them is a real bug.
//
// tc_strip_noise walks `while (*p || first)` and STOPS at end-of-string, so a
// trailing newline yields no final empty line. tc_dedup_lines,
// tc_truncate_with_signal and tc_signal_filter use a `for(;;)` + has_nl walk that
// DOES count the empty segment after a trailing newline as a line.
//
// That difference is load-bearing rather than academic: real tool output almost
// always ends with a newline, so under the wrong model `tail=1` keeps an empty
// final line instead of the last real one, and every elided count shifts by one.
// Verified against the C, which returns "1\n... 3 lines elided ...\n" for
// tc_truncate_with_signal("1\n2\n3\n4\n", 1, 1, NULL).

// splitLinesStripModel is tc_strip_noise's model: no trailing empty line.
func splitLinesStripModel(s string) []string {
	if s == "" {
		return []string{""}
	}
	lines := strings.Split(s, "\n")
	if n := len(lines); n > 1 && lines[n-1] == "" {
		lines = lines[:n-1]
	}
	return lines
}

// splitLines is the model the dedup / truncate / signal-filter family uses: a
// trailing newline DOES produce a final empty line.
func splitLines(s string) []string {
	return strings.Split(s, "\n")
}

// cleanLine drops ANSI CSI escapes and resolves carriage-return redraws.
func cleanLine(line string) string {
	// CR redraw: a terminal rewrites the line after each '\r', so the final
	// rendered state is the text after the LAST '\r'. Keeping the earlier
	// segments would preserve progress spam that was never visible.
	if i := strings.LastIndexByte(line, '\r'); i >= 0 {
		line = line[i+1:]
	}

	var b strings.Builder
	for i := 0; i < len(line); {
		if line[i] == 0x1b && i+1 < len(line) && line[i+1] == '[' {
			// CSI: ESC '[' params (0x20..0x3f) then a final byte (0x40..0x7e).
			j := i + 2
			for j < len(line) && line[j] >= 0x20 && line[j] < 0x40 {
				j++
			}
			if j < len(line) && line[j] >= 0x40 && line[j] <= 0x7e {
				j++ // consume the valid final byte
			}
			// A malformed or truncated CSI still drops, rather than leaking
			// escape bytes into the condensed text.
			i = j
			continue
		}
		b.WriteByte(line[i])
		i++
	}
	return b.String()
}

// TCStripNoise removes content-free noise line-wise: ANSI CSI escapes, CR
// progress redraws, and runs of 2+ blank lines collapsed to one.
//
// Never drops a line that carries text.
func TCStripNoise(in string) string {
	var out []string
	prevBlank := false
	for _, line := range splitLinesStripModel(in) {
		cleaned := cleanLine(line)
		isBlank := cleaned == ""
		if isBlank && prevBlank {
			continue // collapse the run
		}
		out = append(out, cleaned)
		prevBlank = isBlank
	}
	return strings.Join(out, "\n")
}

// TCDedupLines collapses a run of immediately-repeated identical lines to one
// line followed by "  (xN)".
//
// Only IMMEDIATE repeats: identical lines separated by other content are
// distinct events and collapsing them would misrepresent the output.
func TCDedupLines(in string) string {
	lines := splitLines(in)
	var out []string
	i := 0
	for i < len(lines) {
		run := 1
		for i+run < len(lines) && lines[i+run] == lines[i] {
			run++
		}
		if run > 1 {
			out = append(out, fmt.Sprintf("%s  (x%d)", lines[i], run))
		} else {
			out = append(out, lines[i])
		}
		i += run
	}
	return strings.Join(out, "\n")
}

// TCTruncateWithSignal keeps the first head and last tail lines, plus any line
// containing signal, replacing each elided run with an explicit marker.
//
// The marker matters: a silent elision reads as "that is all the output there
// was", which is exactly the misreading that makes condensation dangerous.
// signal is a case-sensitive substring; empty means none forced. head/tail below
// zero are treated as zero, and input that already fits is returned verbatim.
func TCTruncateWithSignal(in string, head, tail int, signal string) string {
	if head < 0 {
		head = 0
	}
	if tail < 0 {
		tail = 0
	}
	lines := splitLines(in)
	if head+tail >= len(lines) {
		return in // already fits
	}

	var out []string
	elided := 0
	flush := func() {
		if elided > 0 {
			out = append(out, fmt.Sprintf("... %d lines elided ...", elided))
			elided = 0
		}
	}
	for i, line := range lines {
		keep := i < head || i >= len(lines)-tail
		if !keep && signal != "" && strings.Contains(line, signal) {
			keep = true
		}
		if keep {
			flush()
			out = append(out, line)
		} else {
			elided++
		}
	}
	flush()
	return strings.Join(out, "\n")
}
