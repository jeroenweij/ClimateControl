package service

import (
	"context"
	"encoding/binary"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// mcBootloader is a byte-level model of MainController's bootloader
// (Software/Modules/MainBootloader/Firmware.cpp) sitting behind the Sender:
// it consumes exactly the frames the driver puts on the uplink and answers
// with the frames the real bootloader would.
type mcBootloader struct {
	svc *Service

	mu        sync.Mutex
	inBoot    bool
	state     uint8
	expected  int
	size      int
	crc32     uint32
	received  []byte
	activated bool
	corrupt   bool // flip a byte in what "flash" holds, to provoke a CRC mismatch
	dropData  int  // swallow this many OtaData frames without answering (lost ack/frame)
	sawSystem bool
}

func (b *mcBootloader) Connected() bool { return true }
func (b *mcBootloader) SendGet(n int, e nodelib.Endpoint) bool {
	return b.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpGet})
}
func (b *mcBootloader) SendSet(n int, e nodelib.Endpoint, d []byte) bool {
	return b.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpSet, Data: d})
}

func (b *mcBootloader) reply(ep nodelib.Endpoint, op nodelib.Operation, data []byte) {
	b.svc.OnOtaFrame(nodelib.Frame{Node: 0, Endpoint: ep, Operation: op, Data: data})
}

func (b *mcBootloader) Send(f nodelib.Frame) bool {
	b.mu.Lock()
	defer b.mu.Unlock()

	switch {
	case f.Endpoint == nodelib.EndpointSystemControl && f.Operation == nodelib.OpSet:
		if !b.inBoot { // the running app parks itself; the bootloader drops it
			b.sawSystem = true
			b.inBoot = true
			b.state = nodelib.BlIdle
		}

	case !b.inBoot:
		// the running app ignores OtaControl / OtaData entirely

	case f.Endpoint == nodelib.EndpointOtaControl && f.Operation == nodelib.OpGet:
		p := make([]byte, 8)
		p[0] = b.state
		binary.LittleEndian.PutUint32(p[1:], uint32(b.expected))
		b.reply(nodelib.EndpointOtaControl, nodelib.OpReport, p)

	case f.Endpoint == nodelib.EndpointOtaControl && f.Operation == nodelib.OpSet:
		switch nodelib.OtaControlOp(f.Data[0]) {
		case nodelib.OtaOpBegin:
			if len(f.Data) != 11 {
				b.reply(nodelib.EndpointOtaControl, nodelib.OpNack, []byte{nodelib.FwErrBadSize})
				return true
			}
			b.size = int(binary.LittleEndian.Uint32(f.Data[1:]))
			b.crc32 = binary.LittleEndian.Uint32(f.Data[5:])
			b.expected, b.received, b.state = 0, nil, nodelib.BlReceiving
			b.reply(nodelib.EndpointOtaControl, nodelib.OpAck, []byte{0})
		case nodelib.OtaOpEnd:
			if b.state != nodelib.BlReceiving || b.expected != b.size {
				b.state = nodelib.BlError
				b.reply(nodelib.EndpointOtaControl, nodelib.OpNack, []byte{nodelib.FwErrBadState})
				return true
			}
			flash := append([]byte(nil), b.received...)
			if b.corrupt {
				flash[10] ^= 0xFF
			}
			got := nodelib.CRC32(flash[:len(flash)-4])
			trailing := binary.LittleEndian.Uint32(flash[len(flash)-4:])
			if got != trailing || got != b.crc32 {
				b.state = nodelib.BlError
				b.reply(nodelib.EndpointOtaControl, nodelib.OpNack, []byte{nodelib.FwErrCrcMismatch})
				return true
			}
			b.state = nodelib.BlValid
			b.reply(nodelib.EndpointOtaControl, nodelib.OpAck, []byte{0})
		case nodelib.OtaOpActivate:
			b.activated = b.state == nodelib.BlValid
		}

	case f.Endpoint == nodelib.EndpointOtaData && f.Operation == nodelib.OpSet:
		if b.dropData > 0 {
			b.dropData--
			return true
		}
		off := int(binary.LittleEndian.Uint16(f.Data[0:]))
		chunk := f.Data[2:]
		p := make([]byte, 5)
		switch {
		case off < b.expected: // duplicate: answered from "flash", never re-programmed
			binary.LittleEndian.PutUint16(p[0:], uint16(off))
			binary.LittleEndian.PutUint16(p[2:], nodelib.CRC16(b.received[off:off+len(chunk)]))
			b.reply(nodelib.EndpointOtaData, nodelib.OpAck, p)
		case off > b.expected:
			binary.LittleEndian.PutUint16(p[0:], uint16(b.expected))
			b.reply(nodelib.EndpointOtaData, nodelib.OpNack, p)
		default:
			b.received = append(b.received, chunk...)
			b.expected += len(chunk)
			binary.LittleEndian.PutUint16(p[0:], uint16(off))
			binary.LittleEndian.PutUint16(p[2:], nodelib.CRC16(chunk))
			b.reply(nodelib.EndpointOtaData, nodelib.OpAck, p)
		}
	}
	return true
}

var _ Sender = (*mcBootloader)(nil)

// mcImage builds a finalized MainController image (descriptor + trailing
// CRC-32) larger than one chunk and not a multiple of 32.
func mcImage(module nodelib.Module) []byte {
	bin := make([]byte, 300)
	for i := range bin {
		bin[i] = byte(i * 7)
	}
	d := bin[0xC0:]
	binary.LittleEndian.PutUint32(d[0:], 0x43436D67)
	binary.LittleEndian.PutUint16(d[4:], 1)
	binary.LittleEndian.PutUint16(d[6:], uint16(module))
	binary.LittleEndian.PutUint32(d[8:], uint32(len(bin)))
	binary.LittleEndian.PutUint16(d[12:], 3)
	binary.LittleEndian.PutUint16(d[14:], 1)
	binary.LittleEndian.PutUint16(d[16:], 1) // CRC present
	binary.LittleEndian.PutUint32(bin[len(bin)-4:], nodelib.CRC32(bin[:len(bin)-4]))
	return bin
}

func runMCPush(t *testing.T, mc *mcBootloader) (state, errMsg string) {
	t.Helper()
	svc, _ := newTestService(t)
	mc.svc = svc
	svc.SetSender(mc)

	id, err := svc.StartOTA(context.Background(), 0, "node", "MainController_3.1.bin", mcImage(nodelib.ModuleMainController), t.TempDir())
	if err != nil {
		t.Fatalf("StartOTA: %v", err)
	}
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		job, err := svc.Store().OtaJob(context.Background(), id)
		if err != nil {
			t.Fatalf("OtaJob: %v", err)
		}
		if job.State == "done" || job.State == "error" {
			return job.State, job.Error
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("push did not finish")
	return
}

func TestMainControllerPushEndToEnd(t *testing.T) {
	mc := &mcBootloader{}
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if !mc.sawSystem {
		t.Error("driver never sent SystemControl[reset->bootloader] to node 0")
	}
	want := mcImage(nodelib.ModuleMainController)
	if string(mc.received) != string(want) {
		t.Errorf("bootloader received %d bytes that differ from the image (%d bytes)", len(mc.received), len(want))
	}
	if !mc.activated {
		t.Error("Activate never reached the bootloader in state Valid")
	}
}

func TestMainControllerPushAlreadyInBootloader(t *testing.T) {
	mc := &mcBootloader{inBoot: true, state: nodelib.BlIdle}
	if state, msg := runMCPush(t, mc); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
}

func TestMainControllerPushRetriesALostChunk(t *testing.T) {
	// One chunk's frame (or its ack) is lost; the resend is answered and the
	// push still completes -- a full otaWriteWait per lost chunk.
	mc := &mcBootloader{dropData: 1}
	if state, msg := runMCPush(t, mc); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
}

func TestMainControllerPushReportsCRCFailure(t *testing.T) {
	mc := &mcBootloader{corrupt: true}
	state, msg := runMCPush(t, mc)
	if state != "error" || !strings.Contains(msg, "final image CRC") {
		t.Fatalf("state = %q msg = %q, want a final-CRC error", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if mc.activated {
		t.Error("a failed image was activated")
	}
}

func TestStartOTAMainControllerImageRules(t *testing.T) {
	svc, _ := newTestService(t)
	dir := t.TempDir()
	if _, err := svc.StartOTA(context.Background(), 0, "node", "cn.bin", image(nodelib.ModuleControllerNode), dir); !errors.Is(err, ErrOtaTargetMismatch) {
		t.Fatalf("CN image to node 0: got %v, want ErrOtaTargetMismatch", err)
	}
	if _, err := svc.StartOTA(context.Background(), 3, "node", "mc.bin", image(nodelib.ModuleMainController), dir); !errors.Is(err, ErrOtaTargetMismatch) {
		t.Fatalf("MC image to node 3: got %v, want ErrOtaTargetMismatch", err)
	}
}

// An UplinkHello with fwVersion 0 is the MainController bootloader: the uplink
// is up and it can be pushed to, but nothing runs the bus, so every bus node
// must read offline -- and the MC row says so.
func TestBootloaderHelloTakesBusNodesOffline(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()
	if err := svc.Store().UpsertNode(ctx, 2, nodelib.ModuleControllerNode, true); err != nil {
		t.Fatal(err)
	}

	status := func() (mc FwTarget, node FwTarget) {
		v, err := svc.FirmwareView(ctx)
		if err != nil {
			t.Fatal(err)
		}
		for _, tg := range v.Targets {
			switch {
			case tg.NodeID == 0:
				mc = tg
			case tg.NodeID == 2 && tg.Target == "node":
				node = tg
			}
		}
		return
	}

	// Application running (non-zero version): master and bus node online.
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0x0001})
	if !svc.MasterOnline() || svc.MasterBootloader() {
		t.Fatal("app hello: want master online, not bootloader")
	}
	mc, node := status()
	if mc.Status != "online" || !node.Online {
		t.Fatalf("app: mc=%q node online=%v", mc.Status, node.Online)
	}

	// Bootloader (version 0): uplink still connected, but not the master.
	svc.OnConnect(nodelib.UplinkHello{FWVersion: 0})
	if svc.MasterOnline() || !svc.MasterBootloader() || !svc.UplinkConnected() {
		t.Fatal("bootloader hello: want uplink up, bootloader, master not online")
	}
	mc, node = status()
	if mc.Status != "bootloader" || !mc.Online || mc.InstalledStr != "?" {
		t.Errorf("mc row = %+v, want status bootloader, online, version ?", mc)
	}
	if node.Online || node.Status == "online" {
		t.Errorf("bus node still %q/online=%v while the MainController is in its bootloader", node.Status, node.Online)
	}

	// Disconnect clears it.
	svc.OnDisconnect()
	if svc.MasterBootloader() {
		t.Error("bootloader flag survived a disconnect")
	}
}
