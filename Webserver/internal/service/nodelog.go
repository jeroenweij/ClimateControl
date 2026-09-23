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

// ReadNodeLog drains a node's DiagLog ring (Node-Message-Model-Spec.md §3):
// one Get per line, oldest first, until the node answers with an empty
// Report. Lines are at most 32 characters, one bus message each; a
// "~ <n> lost" line first means the ring overflowed since it was last read.
func (s *Service) ReadNodeLog(ctx context.Context, node int) ([]string, error) {
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

	lines := []string{}
	for i := 0; i < maxLogReads; i++ {
		if !s.send.SendGet(node, nodelib.EndpointDiagLog) {
			return lines, ErrDownlinkUnavailable
		}
		select {
		case f := <-ch:
			if len(f.Data) == 0 {
				return lines, nil // drained
			}
			lines = append(lines, string(f.Data))
		case <-time.After(diagLogWait):
			return lines, ErrNodeNoReply
		case <-ctx.Done():
			return lines, ctx.Err()
		}
	}
	return lines, nil
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
