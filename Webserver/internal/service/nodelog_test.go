package service

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// logNode answers DiagLog Gets like a node's ring: one line per Get, then an
// empty Report.
type logNode struct {
	fakeSender
	svc   *Service
	lines []string // each "<uptimeSec>|<text>"
	now   uint32   // the node's uptime, sent on the drained report
	gets  int
	mute  bool // never answers
}

func stampedPayload(uptime uint32, text string) []byte {
	return append([]byte{byte(uptime), byte(uptime >> 8), byte(uptime >> 16)}, text...)
}

func (l *logNode) SendGet(n int, e nodelib.Endpoint) bool {
	if e != nodelib.EndpointDiagLog {
		return l.fakeSender.SendGet(n, e)
	}
	l.gets++
	if l.mute {
		return true
	}
	data := stampedPayload(l.now, "") // drained: no text, uptime = now
	if len(l.lines) > 0 {
		var at uint32
		var text string
		fmt.Sscanf(l.lines[0], "%d|", &at)
		text = l.lines[0][strings.Index(l.lines[0], "|")+1:]
		data = stampedPayload(at, text)
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

func TestReadNodeLogDrainsOneLinePerGetInOrderWithAges(t *testing.T) {
	// Three lines logged at uptime 100, 115 and 130 s; the node's clock reads 145 s.
	ln := &logNode{now: 145, lines: []string{"100|~ 2 lost", "115|I: first", "130|W: second"}}
	svc := newLogService(t, ln)

	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil {
		t.Fatal(err)
	}
	want := []LogLine{{"~ 2 lost", 45}, {"I: first", 30}, {"W: second", 15}}
	if len(lines) != len(want) {
		t.Fatalf("lines = %+v, want %+v", lines, want)
	}
	for i := range want {
		if lines[i] != want[i] {
			t.Errorf("line %d = %+v, want %+v", i, lines[i], want[i])
		}
	}
	if ln.gets != 4 { // three lines + the text-less report that says drained
		t.Errorf("sent %d Gets, want 4", ln.gets)
	}
}

func TestReadNodeLogAgesSurviveTheUptimeCounterWrapping(t *testing.T) {
	// Logged just before the 24-bit counter wrapped, read just after.
	ln := &logNode{now: 10, lines: []string{"16777206|I: before wrap"}}
	svc := newLogService(t, ln)
	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil || len(lines) != 1 || lines[0].AgeSec != 20 {
		t.Fatalf("lines = %+v err = %v, want one line aged 20 s", lines, err)
	}
}

func TestReadNodeLogFromANodeWithoutARingIsJustEmpty(t *testing.T) {
	// A node running firmware without the ring answers with an empty Report.
	ln := &oldNode{}
	svc := newLogService2(t, ln)
	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil || len(lines) != 0 {
		t.Fatalf("lines = %+v err = %v, want none", lines, err)
	}
}

func TestReadNodeLogOfAnEmptyRing(t *testing.T) {
	ln := &logNode{}
	svc := newLogService(t, ln)
	lines, err := svc.ReadNodeLog(context.Background(), 3)
	if err != nil || len(lines) != 0 {
		t.Fatalf("lines = %+v err = %v, want none", lines, err)
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
	svc.OnNodeFrame(nodelib.Frame{Node: 3, Endpoint: nodelib.EndpointDiagLog, Operation: nodelib.OpReport, Data: stampedPayload(5, "I: stray")})

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

// oldNode answers DiagLog Gets with an empty payload, as firmware without a
// log ring does.
type oldNode struct {
	fakeSender
	svc *Service
}

func (o *oldNode) SendGet(n int, e nodelib.Endpoint) bool {
	o.svc.OnNodeFrame(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpReport})
	return true
}

func newLogService2(t *testing.T, o *oldNode) *Service {
	t.Helper()
	svc, _ := newTestService(t)
	o.svc = svc
	svc.SetSender(o)
	return svc
}
