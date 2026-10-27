package service

import (
	"context"
	"testing"

	"climatecontrol/webserver/internal/nodelib"
)

// A node whose very first sighting is a live NodePresence (up between two
// Roster dumps) must still get a roster row -- OnPresence used to call a
// plain UPDATE (SetNodeOnline), which silently no-oped against a row that
// didn't exist yet.
func TestOnPresenceCreatesARowForANeverBeforeSeenNode(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()

	svc.OnPresence(nodelib.NodePresence{NodeID: 7, Module: nodelib.ModuleControllerNode, Up: true})

	nodes, err := svc.Store().Nodes(ctx)
	if err != nil {
		t.Fatalf("nodes: %v", err)
	}
	found := false
	for _, n := range nodes {
		if n.ID == 7 {
			found = true
			if !n.Online {
				t.Errorf("node 7 online = false, want true")
			}
		}
	}
	if !found {
		t.Fatalf("node 7 has no row after its first-ever NodePresence")
	}
}

// A Roster lists only the nodes active right now, and no NodePresence down
// follows for one that dropped while the uplink was down -- its end must
// mark every node it left out offline.
func TestRosterEndMarksUnlistedNodesOffline(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()

	for _, id := range []int{1, 5, 6} {
		svc.OnPresence(nodelib.NodePresence{NodeID: uint8(id), Module: nodelib.ModuleTemperatureNode, Up: true})
	}
	svc.OnRosterEntry(nodelib.RosterEntry{NodeID: 5, Module: nodelib.ModuleTemperatureNode})
	svc.OnRosterEnd()

	nodes, err := svc.Store().Nodes(ctx)
	if err != nil {
		t.Fatalf("nodes: %v", err)
	}
	want := map[int]bool{1: false, 5: true, 6: false}
	for _, n := range nodes {
		if w, ok := want[n.ID]; ok && n.Online != w {
			t.Errorf("node %d online = %v, want %v", n.ID, n.Online, w)
		}
	}
}

func TestOnPresenceRecordsBootloaderState(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()

	if err := svc.Store().SetExpectedNode(ctx, 4, nodelib.ModuleControllerNode, "", "", "ui"); err != nil {
		t.Fatalf("set expected: %v", err)
	}
	svc.OnPresence(nodelib.NodePresence{NodeID: 4, Module: nodelib.ModuleControllerNode, Up: true, Bootloader: true})

	roster, err := svc.Store().Roster(ctx, true)
	if err != nil {
		t.Fatalf("roster: %v", err)
	}
	var status string
	for _, n := range roster {
		if n.ID == 4 {
			status = n.Status
		}
	}
	if status != "bootloader" {
		t.Errorf("node 4 status = %q, want bootloader", status)
	}

	// A later presence report back in app mode clears it.
	svc.OnPresence(nodelib.NodePresence{NodeID: 4, Module: nodelib.ModuleControllerNode, Up: true, Bootloader: false})
	roster, err = svc.Store().Roster(ctx, true)
	if err != nil {
		t.Fatalf("roster: %v", err)
	}
	for _, n := range roster {
		if n.ID == 4 {
			status = n.Status
		}
	}
	if status != "online" {
		t.Errorf("node 4 status after app-mode presence = %q, want online", status)
	}
}
