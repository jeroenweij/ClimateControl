package store

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func testStore(t *testing.T) *Store {
	t.Helper()
	st, err := Open(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func statusByID(nodes []RosterNode) map[int]string {
	m := make(map[int]string, len(nodes))
	for _, n := range nodes {
		m[n.ID] = n.Status
	}
	return m
}

func TestRosterClassification(t *testing.T) {
	ctx := context.Background()
	st := testStore(t)

	// Expected roster: nodes 1 and 2.
	if err := st.SyncConfigExpectedNodes(ctx, []ExpectedNode{
		{ID: 1, Module: "ControllerNode", Name: "Living room"},
		{ID: 2, Module: "TemperatureNode"},
	}); err != nil {
		t.Fatalf("sync: %v", err)
	}

	// Node 1 online, node 3 shows up unannounced.
	if err := st.UpsertNode(ctx, 1, nodelib.ModuleControllerNode, true); err != nil {
		t.Fatal(err)
	}
	if err := st.UpsertNode(ctx, 3, nodelib.ModuleThermostat, true); err != nil {
		t.Fatal(err)
	}

	got, err := st.Roster(ctx, true) // master online
	if err != nil {
		t.Fatalf("roster: %v", err)
	}
	st1 := statusByID(got)
	if st1[1] != "online" {
		t.Errorf("node 1 = %q, want online", st1[1])
	}
	if st1[2] != "offline" { // expected but never seen
		t.Errorf("node 2 = %q, want offline", st1[2])
	}
	if st1[3] != "unexpected" { // seen but not in the roster
		t.Errorf("node 3 = %q, want unexpected", st1[3])
	}

	// Named entry carries through.
	for _, n := range got {
		if n.ID == 1 && n.Name != "Living room" {
			t.Errorf("node 1 name = %q", n.Name)
		}
	}

	// Master offline: every expected node flips to offline; unexpected stays.
	got, err = st.Roster(ctx, false)
	if err != nil {
		t.Fatalf("roster: %v", err)
	}
	st2 := statusByID(got)
	if st2[1] != "offline" {
		t.Errorf("node 1 (master down) = %q, want offline", st2[1])
	}
	if st2[3] != "unexpected" {
		t.Errorf("node 3 (master down) = %q, want unexpected", st2[3])
	}
}

func TestSyncConfigExpectedNodesRemoval(t *testing.T) {
	ctx := context.Background()
	st := testStore(t)

	if err := st.SyncConfigExpectedNodes(ctx, []ExpectedNode{{ID: 1}, {ID: 2}}); err != nil {
		t.Fatal(err)
	}
	// A runtime-added node.
	if err := st.SetExpectedNode(ctx, 9, nodelib.ModuleUnknown, "manual", "", "ui"); err != nil {
		t.Fatal(err)
	}
	// Config now only lists node 1.
	if err := st.SyncConfigExpectedNodes(ctx, []ExpectedNode{{ID: 1}}); err != nil {
		t.Fatal(err)
	}

	nodes, err := st.ExpectedNodes(ctx)
	if err != nil {
		t.Fatal(err)
	}
	ids := map[int]string{}
	for _, n := range nodes {
		ids[n.ID] = n.Source
	}
	if _, ok := ids[2]; ok {
		t.Errorf("node 2 should have been removed when dropped from config")
	}
	if ids[1] != "config" {
		t.Errorf("node 1 source = %q, want config", ids[1])
	}
	if ids[9] != "ui" {
		t.Errorf("runtime node 9 should survive a config re-sync, got %q", ids[9])
	}
}
