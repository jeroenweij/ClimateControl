package service

import (
	"fmt"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func TestMainLogLinesArePlacedAgainstTheHello(t *testing.T) {
	svc, _ := newTestService(t)
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0x0100, UptimeSec: 100})
	svc.mu.Lock()
	wall := svc.helloWall
	svc.mu.Unlock()

	svc.OnMainLog(95, "I: before the hello")
	svc.OnMainLog(130, "I: after the hello")

	lines := svc.MainLog()
	if len(lines) != 2 {
		t.Fatalf("got %d lines, want 2", len(lines))
	}
	if want := wall.Add(-5 * time.Second).UnixMilli(); lines[0].TS != want {
		t.Errorf("line 0 ts = %d, want %d (5 s before the hello)", lines[0].TS, want)
	}
	if want := wall.Add(30 * time.Second).UnixMilli(); lines[1].TS != want {
		t.Errorf("line 1 ts = %d, want %d (30 s after the hello)", lines[1].TS, want)
	}
	if lines[0].Text != "I: before the hello" || lines[1].Text != "I: after the hello" {
		t.Errorf("texts = %q, %q", lines[0].Text, lines[1].Text)
	}
}

func TestMainLogWrapsWithTheLines24BitUptime(t *testing.T) {
	svc, _ := newTestService(t)
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0x0100, UptimeSec: 1<<24 + 2}) // 32-bit hello uptime
	svc.mu.Lock()
	wall := svc.helloWall
	svc.mu.Unlock()

	svc.OnMainLog(1<<24-3, "I: just before the wrap") // 5 s before the hello

	if want := wall.Add(-5 * time.Second).UnixMilli(); svc.MainLog()[0].TS != want {
		t.Errorf("ts = %d, want %d", svc.MainLog()[0].TS, want)
	}
}

func TestMainLogKeepsTheMostRecentLines(t *testing.T) {
	svc, _ := newTestService(t)
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0x0100})
	for i := 0; i < mainLogKeep+20; i++ {
		svc.OnMainLog(uint32(i), fmt.Sprintf("I: %d", i))
	}

	lines := svc.MainLog()
	if len(lines) != mainLogKeep {
		t.Fatalf("got %d lines, want %d", len(lines), mainLogKeep)
	}
	if lines[0].Text != "I: 20" || lines[len(lines)-1].Text != fmt.Sprintf("I: %d", mainLogKeep+19) {
		t.Errorf("kept %q .. %q, want the newest %d", lines[0].Text, lines[len(lines)-1].Text, mainLogKeep)
	}
}
