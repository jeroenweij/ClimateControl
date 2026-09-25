package uplink

import (
	"context"
	"io"
	"log/slog"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// recorder is a Handler that just remembers what it was given.
type recorder struct {
	mu       sync.Mutex
	roster   []nodelib.RosterEntry
	presence []nodelib.NodePresence
	connects int
	mainLog  []string
}

func (r *recorder) OnConnect(nodelib.UplinkHello) { r.mu.Lock(); r.connects++; r.mu.Unlock() }
func (r *recorder) OnDisconnect()                 {}
func (r *recorder) OnNodeFrame(nodelib.Frame)     {}
func (r *recorder) OnRosterEntry(e nodelib.RosterEntry) {
	r.mu.Lock()
	r.roster = append(r.roster, e)
	r.mu.Unlock()
}
func (r *recorder) OnPresence(p nodelib.NodePresence) {
	r.mu.Lock()
	r.presence = append(r.presence, p)
	r.mu.Unlock()
}
func (r *recorder) OnMainStatus(nodelib.MainStatus)             {}
func (r *recorder) OnThermostatStatus(nodelib.ThermostatStatus) {}
func (r *recorder) OnOtaFrame(nodelib.Frame)                    {}
func (r *recorder) OnMainLog(uptimeSec uint32, text string) {
	r.mu.Lock()
	r.mainLog = append(r.mainLog, text)
	r.mu.Unlock()
}

func (r *recorder) rosterLen() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.roster)
}

func encode(t *testing.T, f nodelib.Frame) []byte {
	t.Helper()
	raw, err := nodelib.Encode(f)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func hello(token [16]byte) nodelib.Frame {
	data := make([]byte, 23)
	copy(data[7:], token[:])
	return nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointUplinkHello, Operation: nodelib.OpReport, Data: data}
}

func startServer(t *testing.T, h Handler, token [16]byte) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	s := New(token, h, nil, slog.New(slog.NewTextHandler(io.Discard, nil)))
	go func() { _ = s.ListenAndServe(ctx, addr) }()
	for i := 0; i < 100; i++ {
		if c, err := net.Dial("tcp", addr); err == nil {
			c.Close()
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	return addr
}

func TestFramesSentWithTheHelloAreNotLost(t *testing.T) {
	// The MainController writes its hello and the roster back to back and the
	// module usually delivers them as one TCP segment. The roster is what tells
	// the server which nodes are in their bootloader, so it must survive the
	// handshake.
	token := [16]byte{1, 2, 3}
	rec := &recorder{}
	addr := startServer(t, rec, token)

	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	var burst []byte
	burst = append(burst, encode(t, hello(token))...)
	for _, e := range []nodelib.Frame{
		{Node: 0, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpReport, Data: []byte{1, 2, 1, 0x02, 0x26, 0, 0}},
		{Node: 0, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpReport, Data: []byte{2, 1, 1, 0x01, 0x26, 0, 0}},
		{Node: 0, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpReport, Data: []byte{0xFF, 0, 0, 0, 0, 0, 0}},
	} {
		burst = append(burst, encode(t, e)...)
	}
	if _, err := c.Write(burst); err != nil { // one write: one segment on loopback
		t.Fatal(err)
	}

	deadline := time.Now().Add(3 * time.Second)
	for rec.rosterLen() < 2 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	rec.mu.Lock()
	defer rec.mu.Unlock()
	if len(rec.roster) != 2 {
		t.Fatalf("roster entries received = %d, want 2 (frames sent with the hello were dropped)", len(rec.roster))
	}
	if rec.roster[0].NodeID != 1 || rec.roster[0].State != 1 || rec.roster[1].NodeID != 2 || rec.roster[1].State != 1 {
		t.Errorf("roster = %+v, want nodes 1 and 2 both in the bootloader", rec.roster)
	}
}

func TestMainLogReportsReachTheHandler(t *testing.T) {
	token := [16]byte{4, 5, 6}
	rec := &recorder{}
	addr := startServer(t, rec, token)

	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	var burst []byte
	burst = append(burst, encode(t, hello(token))...)
	burst = append(burst, encode(t, nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointMainLog, Operation: nodelib.OpReport,
		Data: append([]byte{7, 0, 0}, "I: Uplink: data mode"...)})...)
	if _, err := c.Write(burst); err != nil {
		t.Fatal(err)
	}

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		rec.mu.Lock()
		n := len(rec.mainLog)
		rec.mu.Unlock()
		if n > 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	rec.mu.Lock()
	defer rec.mu.Unlock()
	if len(rec.mainLog) != 1 || rec.mainLog[0] != "I: Uplink: data mode" {
		t.Fatalf("main log = %q, want the one line", rec.mainLog)
	}
}
