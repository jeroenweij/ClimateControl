package service

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func sentGets(fs *fakeSender, node uint8) []nodelib.Endpoint {
	var eps []nodelib.Endpoint
	for _, f := range fs.Frames() {
		if f.Node == node && f.Operation == nodelib.OpGet {
			eps = append(eps, f.Endpoint)
		}
	}
	return eps
}

func TestConnectOfTheMainControllerAppAsksForItsStatusAndLastFault(t *testing.T) {
	svc, fs := newTestService(t)
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0x0301})

	got := sentGets(fs, 0)
	if len(got) != 2 || got[0] != nodelib.EndpointSystemStatus || got[1] != nodelib.EndpointDiagLastError {
		t.Fatalf("Gets to node 0 = %v, want SystemStatus then DiagLastError", got)
	}
}

func TestConnectOfTheMainControllerBootloaderAsksNothing(t *testing.T) {
	svc, fs := newTestService(t)
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0}) // the bootloader answers neither

	if got := sentGets(fs, 0); len(got) != 0 {
		t.Fatalf("Gets to node 0 = %v, want none", got)
	}
}

func TestMainControllerLastFaultIsStoredAndPublishedButNotARosterNode(t *testing.T) {
	svc, fs := newTestService(t)
	ctx := context.Background()

	// HardFault at PC 0x08002A5C, 123.456 s after boot.
	svc.OnNodeFrame(nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointDiagLastError, Operation: nodelib.OpReport,
		Data: []byte{nodelib.FaultHardFault, 0x40, 0xE2, 0x01, 0x00, 0x5C, 0x2A}})

	rows, err := svc.Store().Readings(ctx, 0, nodelib.EndpointDiagLastError, 0, time.Now().Add(time.Hour).UnixMilli(), 10)
	if err != nil || len(rows) != 1 {
		t.Fatalf("stored = %v, %v; want one row under node 0", rows, err)
	}
	snap := string(svc.Hub().SnapshotJSON())
	if !strings.Contains(snap, `"codeName":"HardFault"`) || !strings.Contains(snap, `"context":10844`) {
		t.Errorf("snapshot does not carry the decoded fault: %s", snap)
	}
	nodes, err := svc.Store().Nodes(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(nodes) != 0 {
		t.Errorf("roster = %+v, want the MainController kept out of it", nodes)
	}
	if got := sentGets(fs, 0); len(got) != 0 {
		t.Errorf("Gets to node 0 = %v, want no SystemInfo probe", got)
	}
}
