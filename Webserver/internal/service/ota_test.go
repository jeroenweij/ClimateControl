package service

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log/slog"
	"path/filepath"
	"testing"

	"github.com/jweij/climatecontrol/webserver/internal/hub"
	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	"github.com/jweij/climatecontrol/webserver/internal/store"
)

type fakeSender struct{ frames []nodelib.Frame }

func (f *fakeSender) Connected() bool            { return true }
func (f *fakeSender) Send(fr nodelib.Frame) bool { f.frames = append(f.frames, fr); return true }
func (f *fakeSender) SendGet(n int, e nodelib.Endpoint) bool {
	return f.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpGet})
}
func (f *fakeSender) SendSet(n int, e nodelib.Endpoint, d []byte) bool {
	return f.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpSet, Data: d})
}

// image builds a minimal valid .bin for the given module: descriptor at 0xC0,
// magic 'CCmg', no CRC-present flag.
func image(module nodelib.Module) []byte {
	bin := make([]byte, 0x100)
	d := bin[0xC0:]
	binary.LittleEndian.PutUint32(d[0:], 0x43436D67) // 'CCmg'
	binary.LittleEndian.PutUint16(d[4:], 1)          // header version
	binary.LittleEndian.PutUint16(d[6:], uint16(module))
	binary.LittleEndian.PutUint32(d[8:], uint32(len(bin))) // image size
	binary.LittleEndian.PutUint16(d[12:], 2)               // fw major
	binary.LittleEndian.PutUint16(d[14:], 5)               // fw minor
	return bin
}

func newTestService(t *testing.T) (*Service, *fakeSender) {
	t.Helper()
	st, err := store.Open(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("store: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	svc := New(st, hub.New(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	fs := &fakeSender{}
	svc.SetSender(fs)
	return svc, fs
}

func TestStartOTATargetValidation(t *testing.T) {
	svc, _ := newTestService(t)
	dir := t.TempDir()

	// Thermostat image sent to the node itself -> mismatch.
	if _, err := svc.StartOTA(context.Background(), 3, "node", "therm.bin", image(nodelib.ModuleThermostat), dir); !errors.Is(err, ErrOtaTargetMismatch) {
		t.Fatalf("therm image / node target: got %v, want ErrOtaTargetMismatch", err)
	}
	// ControllerNode image sent as a thermostat push -> mismatch.
	if _, err := svc.StartOTA(context.Background(), 3, "thermostat", "cn.bin", image(nodelib.ModuleControllerNode), dir); !errors.Is(err, ErrOtaTargetMismatch) {
		t.Fatalf("CN image / thermostat target: got %v, want ErrOtaTargetMismatch", err)
	}
	// Unknown target string -> mismatch.
	if _, err := svc.StartOTA(context.Background(), 3, "bogus", "x.bin", image(nodelib.ModuleControllerNode), dir); !errors.Is(err, ErrOtaTargetMismatch) {
		t.Fatalf("bogus target: got %v, want ErrOtaTargetMismatch", err)
	}
}

func TestStartOTAThermostatJob(t *testing.T) {
	svc, _ := newTestService(t)

	jobID, err := svc.StartOTA(context.Background(), 3, "thermostat", "therm.bin", image(nodelib.ModuleThermostat), t.TempDir())
	if err != nil {
		t.Fatalf("StartOTA: %v", err)
	}
	job, err := svc.Store().OtaJob(context.Background(), jobID)
	if err != nil {
		t.Fatalf("OtaJob: %v", err)
	}
	if job.Target != "thermostat" {
		t.Errorf("job target = %q, want thermostat", job.Target)
	}
	if job.NodeID != 3 {
		t.Errorf("job nodeId = %d, want 3 (the ControllerNode)", job.NodeID)
	}
	// module byte carried in 0x65 OtaControl must be Thermostat so the
	// MainController routes to ThermostatFirmware.
	if job.Module != int(nodelib.ModuleThermostat) {
		t.Errorf("job module = %d, want %d (Thermostat)", job.Module, nodelib.ModuleThermostat)
	}

	// A second push while this one is live is rejected.
	if _, err := svc.StartOTA(context.Background(), 4, "node", "x.bin", image(nodelib.ModuleControllerNode), t.TempDir()); !errors.Is(err, ErrOtaBusy) {
		t.Fatalf("second push: got %v, want ErrOtaBusy", err)
	}
}

var _ Sender = (*fakeSender)(nil)
