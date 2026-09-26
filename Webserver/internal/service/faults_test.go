package service

import (
	"reflect"
	"testing"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func statusFrame(node int, errorFlags uint16) nodelib.Frame {
	return nodelib.Frame{
		Node: uint8(node), Endpoint: nodelib.EndpointSystemStatus, Operation: nodelib.OpReport,
		Data: []byte{0, 0, 0, 0, 0, byte(errorFlags), byte(errorFlags >> 8), 0},
	}
}

func TestFaultsTrackActiveAndKeepTheLastOneAfterItClears(t *testing.T) {
	svc, _ := newTestService(t)
	svc.OnPresence(nodelib.NodePresence{NodeID: 3, Module: nodelib.ModuleControllerNode, Up: true})

	svc.OnNodeFrame(statusFrame(3, 0))
	if f := svc.Faults(3); len(f.Active) != 0 || f.Last != "" {
		t.Fatalf("healthy node: got %+v", f)
	}

	svc.OnNodeFrame(statusFrame(3, 0x0002))
	f := svc.Faults(3)
	if !reflect.DeepEqual(f.Active, []string{"Damper stalled"}) || f.Last != "Damper stalled" || f.LastTS == 0 {
		t.Fatalf("stalled: got %+v", f)
	}

	svc.OnNodeFrame(statusFrame(3, 0))
	f = svc.Faults(3)
	if len(f.Active) != 0 || f.Last != "Damper stalled" {
		t.Fatalf("cleared: want no active fault but the stall kept as last, got %+v", f)
	}
}
