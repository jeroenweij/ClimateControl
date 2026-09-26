package service

import (
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func TestRosterRefillsOnlyTheStateTheCacheIsMissing(t *testing.T) {
	svc, fs := newTestService(t)
	svc.refillGap = 0
	// DamperActual already cached -- must not be asked for again.
	svc.OnNodeFrame(nodelib.Frame{Node: 3, Endpoint: nodelib.EndpointDamperActual, Operation: nodelib.OpReport, Data: []byte{50}})

	svc.OnRosterEntry(nodelib.RosterEntry{NodeID: 3, Module: nodelib.ModuleControllerNode})

	want := map[nodelib.Endpoint]bool{
		nodelib.EndpointDamperTarget: true, nodelib.EndpointDamperMode: true, nodelib.EndpointRoomTemp: true,
		nodelib.EndpointRoomSetpoint: true, nodelib.EndpointSystemStatus: true,
	}
	got := waitForGets(t, fs, 3, len(want))
	for ep := range want {
		if !got[ep] {
			t.Errorf("no refill Get for %v", ep)
		}
	}
	if got[nodelib.EndpointDamperActual] {
		t.Errorf("refilled DamperActual although it was cached")
	}
}

func TestPresenceUpRefillsAllStateForAModule(t *testing.T) {
	svc, fs := newTestService(t)
	svc.refillGap = 0

	svc.OnPresence(nodelib.NodePresence{NodeID: 1, Module: nodelib.ModuleTemperatureNode, Up: true})

	got := waitForGets(t, fs, 1, 3)
	for _, ep := range []nodelib.Endpoint{nodelib.EndpointSupplyTemp, nodelib.EndpointReturnTemp, nodelib.EndpointSystemStatus} {
		if !got[ep] {
			t.Errorf("no refill Get for %v", ep)
		}
	}
}

// waitForGets waits (the refill worker is asynchronous) until at least n
// refill Gets for node have been sent -- SystemInfo, sent directly, doesn't
// count -- and returns which endpoints were asked for.
func waitForGets(t *testing.T, fs *fakeSender, node int, n int) map[nodelib.Endpoint]bool {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		got := map[nodelib.Endpoint]bool{}
		for _, f := range fs.Frames() {
			if int(f.Node) == node && f.Operation == nodelib.OpGet && f.Endpoint != nodelib.EndpointSystemInfo {
				got[f.Endpoint] = true
			}
		}
		if len(got) >= n || time.Now().After(deadline) {
			time.Sleep(20 * time.Millisecond) // and let any stray extra land too
			return got
		}
		time.Sleep(5 * time.Millisecond)
	}
}
