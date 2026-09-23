package service

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// logNode answers DiagLog Gets like a node's ring: one line per Get, then an
// empty Report.
type logNode struct {
	fakeSender
	svc   *Service
	lines []string
	gets  int
	mute  bool // never answers
}

func (l *logNode) SendGet(n int, e nodelib.Endpoint) bool {
	if e != nodelib.EndpointDiagLog {
		return l.fakeSender.SendGet(n, e)
	}
	l.gets++
	if l.mute {
		return true
	}
	var data []byte
	if len(l.lines) > 0 {
		data = []byte(l.lines[0])
		l.lines = l.lines[1:]
	}
	l.svc.OnNodeFrame(nodelib.Frame{Node: uint8(n), Endpoint: nodelib.EndpointDiagLog, Operation: nodelib.OpReport, Data: data})
	return true
}

func newLogService(t *testing.T, ln *logNode) *Service {
	t.Helper()
	svc, _ := newTestService(t)
	ln.svc = svc
	svc.SetSender(ln)
	return svc
}

func TestReadNodeLogDrainsOneLinePerGetInOrder(t *testing.T) {
	ln := &logNode{lines: []string{"~ 2 lost", "I: first", "W: second"}}
	svc := newLogService(t, ln)

	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"~ 2 lost", "I: first", "W: second"}
	if len(lines) != len(want) {
		t.Fatalf("lines = %q, want %q", lines, want)
	}
	for i := range want {
		if lines[i] != want[i] {
			t.Errorf("line %d = %q, want %q", i, lines[i], want[i])
		}
	}
	if ln.gets != 4 { // three lines + the empty Report that says drained
		t.Errorf("sent %d Gets, want 4", ln.gets)
	}
}

func TestReadNodeLogOfAnEmptyRing(t *testing.T) {
	ln := &logNode{}
	svc := newLogService(t, ln)
	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil || len(lines) != 0 {
		t.Fatalf("lines = %q err = %v, want none", lines, err)
	}
}

func TestReadNodeLogTimesOutWhenTheNodeIsSilent(t *testing.T) {
	old := diagLogWait
	diagLogWait = 30 * time.Millisecond
	t.Cleanup(func() { diagLogWait = old })

	ln := &logNode{mute: true}
	svc := newLogService(t, ln)
	if _, err := svc.ReadNodeLog(context.Background(), 3); !errors.Is(err, ErrNodeNoReply) {
		t.Fatalf("err = %v, want ErrNodeNoReply", err)
	}
	// The reader deregistered: a second read is allowed, not "busy".
	if _, err := svc.ReadNodeLog(context.Background(), 3); !errors.Is(err, ErrNodeNoReply) {
		t.Fatalf("second read err = %v, want ErrNodeNoReply", err)
	}
}

func TestDiagLogReportsAreNotStoredAsReadings(t *testing.T) {
	ln := &logNode{}
	svc := newLogService(t, ln)
	// A log line with nobody waiting for it is dropped, never written to the
	// readings store or published as a sensor value.
	svc.OnNodeFrame(nodelib.Frame{Node: 3, Endpoint: nodelib.EndpointDiagLog, Operation: nodelib.OpReport, Data: []byte("I: stray")})

	rows, err := svc.Store().Readings(context.Background(), 3, nodelib.EndpointDiagLog, 0, time.Now().Add(time.Hour).UnixMilli(), 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 0 {
		t.Fatalf("a DiagLog report was stored as %d reading(s)", len(rows))
	}
}

func TestReadNodeLogRefusesWithoutAMainController(t *testing.T) {
	svc, _ := newTestService(t)
	svc.SetSender(&offlineSender{})
	if _, err := svc.ReadNodeLog(context.Background(), 3); !errors.Is(err, ErrDownlinkUnavailable) {
		t.Fatalf("err = %v, want ErrDownlinkUnavailable", err)
	}
}

type offlineSender struct{ fakeSender }

func (o *offlineSender) Connected() bool { return false }
