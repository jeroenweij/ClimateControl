package service

import (
	"context"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// imageV builds a minimal valid .bin whose descriptor names module + version.
func imageV(module nodelib.Module, major, minor uint16) []byte {
	bin := make([]byte, 0x100)
	d := bin[0xC0:]
	binary.LittleEndian.PutUint32(d[0:], 0x43436D67)
	binary.LittleEndian.PutUint16(d[4:], 1)
	binary.LittleEndian.PutUint16(d[6:], uint16(module))
	binary.LittleEndian.PutUint32(d[8:], uint32(len(bin)))
	binary.LittleEndian.PutUint16(d[12:], major)
	binary.LittleEndian.PutUint16(d[14:], minor)
	return bin
}

func TestStoreFirmwareNamingGate(t *testing.T) {
	svc, _ := newTestService(t)
	repo := t.TempDir()
	ctx := context.Background()

	// Bad filename -> rejected.
	if _, err := svc.StoreFirmware(ctx, "firmware.bin", imageV(nodelib.ModuleControllerNode, 1, 0), repo); !errors.Is(err, ErrBadFirmwareName) {
		t.Fatalf("bad name: got %v, want ErrBadFirmwareName", err)
	}

	// Filename says version 1.0 but the descriptor says 2.0 -> rejected.
	if _, err := svc.StoreFirmware(ctx, "ControllerNode_1.0.bin", imageV(nodelib.ModuleControllerNode, 2, 0), repo); !errors.Is(err, ErrFirmwareDescMismatch) {
		t.Fatalf("desc mismatch: got %v, want ErrFirmwareDescMismatch", err)
	}

	// Good upload.
	fi, err := svc.StoreFirmware(ctx, "ControllerNode_1.0.bin", imageV(nodelib.ModuleControllerNode, 1, 0), repo)
	if err != nil {
		t.Fatalf("StoreFirmware: %v", err)
	}
	if fi.Version != 1<<8 || fi.Module != "ControllerNode" {
		t.Fatalf("stored %+v", fi)
	}
	first := fi.ImagePath
	if _, err := os.Stat(first); err != nil {
		t.Fatalf("image file: %v", err)
	}

	// A newer image for the same module replaces the row; only one file remains.
	if _, err := svc.StoreFirmware(ctx, "ControllerNode_1.2.bin", imageV(nodelib.ModuleControllerNode, 1, 2), repo); err != nil {
		t.Fatalf("replace: %v", err)
	}
	if _, err := os.Stat(first); !os.IsNotExist(err) {
		t.Errorf("old image file should have been removed")
	}
	entries, _ := os.ReadDir(repo)
	if len(entries) != 1 {
		t.Errorf("repo has %d files, want 1", len(entries))
	}
	held, ok, _ := svc.Store().FirmwareImage(ctx, nodelib.ModuleControllerNode)
	if !ok || held.Version != 1<<8|2 {
		t.Errorf("held version = %d, want %d", held.Version, 1<<8|2)
	}
	_ = filepath.Base(first)
}

func TestFirmwareViewMainController(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()
	repo := t.TempDir()

	// Before the MainController ever connects: a row exists but no version.
	view, err := svc.FirmwareView(ctx)
	if err != nil {
		t.Fatal(err)
	}
	mc := view.Targets[0]
	if mc.NodeID != 0 || mc.Module != "MainController" || mc.Target != "node" {
		t.Fatalf("first row is not the MainController: %+v", mc)
	}
	if mc.CanUpdate {
		t.Errorf("no image held yet, should not be updatable")
	}

	// It connects reporting 1.0; a 1.1 image is uploaded.
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 1 << 8})
	if _, err := svc.StoreFirmware(ctx, "MainController_1.1.bin", imageV(nodelib.ModuleMainController, 1, 1), repo); err != nil {
		t.Fatal(err)
	}
	view, _ = svc.FirmwareView(ctx)
	mc = view.Targets[0]
	if !mc.CanUpdate || mc.InstalledStr != "1.0" || mc.LatestStr != "1.1" {
		t.Fatalf("MainController row = %+v, want updatable 1.0 -> 1.1", mc)
	}

	// A push is accepted as a self-update (targetNodeId 0).
	id, err := svc.EnqueueUpdate(ctx, 0, "node", t.TempDir())
	if err != nil {
		t.Fatalf("EnqueueUpdate(0): %v", err)
	}
	job, _ := svc.Store().OtaJob(ctx, id)
	if job.NodeID != 0 || job.Module != int(nodelib.ModuleMainController) {
		t.Fatalf("job = %+v, want node 0 / module MainController", job)
	}
}

func TestFirmwareViewClassification(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()
	repo := t.TempDir()

	if err := svc.Store().SyncConfigExpectedNodes(ctx, nil); err != nil {
		t.Fatal(err)
	}
	if err := svc.Store().SetExpectedNode(ctx, 1, nodelib.ModuleControllerNode, "Living room", "", "ui"); err != nil {
		t.Fatal(err)
	}
	// Node 1 online, running 1.0.
	if err := svc.Store().UpsertNode(ctx, 1, nodelib.ModuleControllerNode, true); err != nil {
		t.Fatal(err)
	}
	if err := svc.Store().SetNodeFirmware(ctx, 1, 1<<8); err != nil {
		t.Fatal(err)
	}
	// Hold a newer ControllerNode image.
	if _, err := svc.StoreFirmware(ctx, "ControllerNode_1.1.bin", imageV(nodelib.ModuleControllerNode, 1, 1), repo); err != nil {
		t.Fatal(err)
	}

	view, err := svc.FirmwareView(ctx)
	if err != nil {
		t.Fatalf("FirmwareView: %v", err)
	}
	// Expect a node row (updatable: online, 1.0 < 1.1) and a thermostat row
	// (not updatable: link down, no image).
	var nodeRow, thermRow *FwTarget
	for i := range view.Targets {
		switch view.Targets[i].Target {
		case "node":
			nodeRow = &view.Targets[i]
		case "thermostat":
			thermRow = &view.Targets[i]
		}
	}
	if nodeRow == nil || !nodeRow.CanUpdate {
		t.Fatalf("node row = %+v, want CanUpdate", nodeRow)
	}
	if thermRow == nil || thermRow.CanUpdate {
		t.Fatalf("thermostat row = %+v, want !CanUpdate", thermRow)
	}

	// Master offline: node flips to offline, button greys out.
	// (MasterOnline is false because the fake sender is Connected but no
	// uplink hello — actually fakeSender.Connected()==true, so simulate by
	// checking the up-to-date path instead.)
	if err := svc.Store().SetNodeFirmware(ctx, 1, 1<<8|1); err != nil {
		t.Fatal(err)
	}
	view, _ = svc.FirmwareView(ctx)
	for _, tg := range view.Targets {
		if tg.Target == "node" && tg.CanUpdate {
			t.Errorf("node now on latest, still CanUpdate: %+v", tg)
		}
	}
}
