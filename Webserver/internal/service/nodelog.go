package service

import (
	"context"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// diagLogWait is how long ReadNodeLog waits for a node to answer one DiagLog
// Get -- the reply rides the node's next poll window. A variable so tests can
// shorten it.
var diagLogWait = 3 * time.Second

// maxLogReads bounds one drain: the node's ring holds 10 lines plus at most a
// "lost" marker, and a node logging faster than we read could otherwise keep
// the loop going.
const maxLogReads = 32

// LogLine is one line from a node's log ring.
type LogLine struct {
	Text string `json:"text"`
	// AgeSec is how long before the read the node logged the line. The lines
	// carry the node's uptime, not wall-clock time, so a backlog read in one
	// go still shows when each line really happened.
	AgeSec int `json:"ageSec"`
}

// uptimeMask is the 24-bit uptime counter DiagLog reports carry.
const uptimeMask = 0xFFFFFF

// ReadNodeLog drains a node's DiagLog ring (Node-Message-Model-Spec.md §3):
// one Get per line, oldest first, until the node answers with a text-less
// Report, whose uptime is the node's "now". Lines are at most 32 characters,
// one bus message each; a "~ <n> lost" line first means the ring overflowed
// since it was last read.
func (s *Service) ReadNodeLog(ctx context.Context, node int) ([]LogLine, error) {
	if !s.MasterOnline() {
		return nil, ErrDownlinkUnavailable
	}

	ch := make(chan nodelib.Frame, 4)
	s.mu.Lock()
	if s.diagLog[node] != nil {
		s.mu.Unlock()
		return nil, ErrLogReadBusy
	}
	s.diagLog[node] = ch
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.diagLog, node)
		s.mu.Unlock()
	}()

	type stamped struct {
		at   uint32
		text string
	}
	var got []stamped
	var now uint32
	haveNow := false

	finish := func() []LogLine {
		if !haveNow && len(got) > 0 {
			// Never saw the drained report (timeout / read limit): take the
			// newest line as "now" -- ages are then relative to it.
			now = got[len(got)-1].at
		}
		lines := make([]LogLine, len(got))
		for i, g := range got {
			lines[i] = LogLine{Text: g.text, AgeSec: int((now - g.at) & uptimeMask)}
		}
		return lines
	}

	for i := 0; i < maxLogReads; i++ {
		if !s.send.SendGet(node, nodelib.EndpointDiagLog) {
			return finish(), ErrDownlinkUnavailable
		}
		select {
		case f := <-ch:
			at, text, ok := nodelib.ParseDiagLog(f.Data)
			if !ok || text == "" {
				now, haveNow = at, ok // drained (or a node without a ring: empty report)
				return finish(), nil
			}
			got = append(got, stamped{at, text})
		case <-time.After(diagLogWait):
			return finish(), ErrNodeNoReply
		case <-ctx.Done():
			return finish(), ctx.Err()
		}
	}
	return finish(), nil
}

// onDiagLog hands a DiagLog Report to the ReadNodeLog waiting on that node.
// With no reader it is dropped: a log line is only ever a reply to our Get.
func (s *Service) onDiagLog(f nodelib.Frame) {
	s.mu.Lock()
	ch := s.diagLog[int(f.Node)]
	s.mu.Unlock()
	if ch == nil {
		return
	}
	select {
	case ch <- f:
	default:
	}
}
