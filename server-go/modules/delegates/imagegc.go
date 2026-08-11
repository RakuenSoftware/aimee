package delegates

import (
	"sort"
	"strconv"
	"strings"
)

// Which built sandbox images may be deleted.
//
// Parsing the timestamps, ORDERING by recency, and judging each image are one
// rule, because the judgement is positional: "keep the keep_min most recent"
// has no meaning until the images are ordered, and the order comes from the
// timestamps. Split across a boundary, a caller that sorted differently would
// silently protect the wrong images while every individual verdict still looked
// correct.
//
// Nothing here deletes anything. The caller runs the removals and reports what
// actually happened -- an image can race back into use between the decision and
// the delete.

// SandboxImage is one built image as the runtime reports it.
type SandboxImage struct {
	// Tag identifies the image.
	Tag string
	// CreatedAt is the runtime's timestamp, e.g. "2026-07-15 12:34:56 +0000 UTC".
	CreatedAt string
	// InUse is whether a container (running or stopped) references it. The
	// caller establishes this; the module does not inspect containers.
	InUse bool
}

// SandboxImageVerdict is what to do with one image, in the caller's original
// order.
type SandboxImageVerdict struct {
	Tag string
	// Remove is the decision.
	Remove bool
	// Reason is the operator-facing word for why, and is reported whether or
	// not the image is removed: "why is this still here" is asked as often as
	// "why did this go".
	Reason string
}

// ParseCreatedEpoch reads a runtime CreatedAt field into a UTC epoch.
//
// Docker emits the local daemon's wall clock with an explicit offset
// ("2026-07-15 12:34:56 +0000 UTC"), so the offset is applied here rather than
// trusting the process's own zone.
//
// Returns ok=false when the leading "Y-M-D H:M:S" does not parse. An image
// whose timestamp cannot be read is treated as OLD: it sorts last, so the
// keep_min protection does not cover it, and it ages out. That is deliberate
// and safe here because these images are a rebuildable cache -- deleting one
// costs a rebuild, whereas keeping every unreadable entry forever would let a
// runtime that changed its format quietly fill the disk.
func ParseCreatedEpoch(created string) (int64, bool) {
	f := strings.Fields(strings.TrimSpace(created))
	if len(f) < 2 {
		return 0, false
	}
	date, clock := f[0], f[1]

	dparts := strings.Split(date, "-")
	tparts := strings.Split(clock, ":")
	if len(dparts) != 3 || len(tparts) != 3 {
		return 0, false
	}
	nums := make([]int, 0, 6)
	for _, s := range append(append([]string{}, dparts...), tparts...) {
		v, err := strconv.Atoi(s)
		if err != nil {
			return 0, false
		}
		nums = append(nums, v)
	}
	y, mo, d, h, mi, sec := nums[0], nums[1], nums[2], nums[3], nums[4], nums[5]
	if mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60 {
		return 0, false
	}

	epoch := daysFromCivil(y, mo, d)*86400 + int64(h)*3600 + int64(mi)*60 + int64(sec)

	// An explicit numeric offset, when present, is what makes this UTC.
	if len(f) >= 3 && len(f[2]) == 5 && (f[2][0] == '+' || f[2][0] == '-') {
		oh, err1 := strconv.Atoi(f[2][1:3])
		om, err2 := strconv.Atoi(f[2][3:5])
		if err1 == nil && err2 == nil {
			off := int64(oh)*3600 + int64(om)*60
			if f[2][0] == '+' {
				epoch -= off
			} else {
				epoch += off
			}
		}
	}
	return epoch, true
}

// daysFromCivil converts a civil date to days since the Unix epoch (Howard
// Hinnant's algorithm). Written out rather than using time.Date so the result
// cannot depend on the process's location database.
func daysFromCivil(y, m, d int) int64 {
	yy := int64(y)
	if m <= 2 {
		yy--
	}
	era := yy / 400
	if yy < 0 {
		era = (yy - 399) / 400
	}
	yoe := yy - era*400
	mp := int64((m + 9) % 12)
	doy := (153*mp+2)/5 + int64(d) - 1
	doe := yoe*365 + yoe/4 - yoe/100 + doy
	return era*146097 + doe - 719468
}

// JudgeSandboxImages decides which images may be deleted.
//
// The order of the returned verdicts matches the input. Recency ordering is
// computed here and used only for the keep_min protection, so the caller never
// has to reproduce it.
func JudgeSandboxImages(images []SandboxImage, now int64, keepMin int,
	maxAgeSecs int64) []SandboxImageVerdict {
	if keepMin < 0 {
		keepMin = 0
	}
	if maxAgeSecs < 0 {
		maxAgeSecs = 0
	}

	type ranked struct {
		idx   int
		epoch int64
		known bool
	}
	order := make([]ranked, len(images))
	for i, img := range images {
		e, ok := ParseCreatedEpoch(img.CreatedAt)
		order[i] = ranked{idx: i, epoch: e, known: ok}
	}
	// Most recent first, stable so equal timestamps keep the caller's order
	// rather than shuffling which image the keep_min protection covers.
	sort.SliceStable(order, func(a, b int) bool { return order[a].epoch > order[b].epoch })

	rank := make([]int, len(images))
	for position, r := range order {
		rank[r.idx] = position
	}

	verdicts := make([]SandboxImageVerdict, len(images))
	for i, img := range images {
		v := SandboxImageVerdict{Tag: img.Tag}
		switch {
		case img.InUse:
			v.Reason = "in-use"
		case rank[i] < keepMin:
			v.Reason = "kept-recent"
		case order[rank[i]].known && now-order[rank[i]].epoch < maxAgeSecs:
			v.Reason = "within-max-age"
		default:
			v.Remove = true
			v.Reason = "aged-out"
		}
		verdicts[i] = v
	}
	return verdicts
}
