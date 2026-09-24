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

	mu         sync.Mutex
	inBoot     bool
	state      uint8
	expected   int
	size       int
	crc32      uint32
	received   []byte
	activated  bool
	corrupt    bool          // flip a byte in what "flash" holds, to provoke a CRC mismatch
	dropData   int           // swallow this many OtaData frames without answering (lost ack/frame)
	dropAcks   int           // apply this many OtaData frames but lose their acks
	restartAt  int           // after this many chunks, the bootloader restarts and loses its state
	silent     bool          // the link is gone: nothing is answered at all
	silentAt   int           // the link goes down after this many chunks are accepted (that chunk's ack is lost too)
	gets       int           // OtaControl Get probes seen
	chunks     int           // chunks accepted so far
	latency    time.Duration // deliver OtaData replies this long after the request (0 = at once)
	dataFrames int           // OtaData requests received (resends included)
	inFlight   int           // requests sent whose reply has not been delivered yet
	maxFlight  int
	sawSystem  bool
}

func (b *mcBootloader) Connected() bool { return true }
func (b *mcBootloader) SendGet(n int, e nodelib.Endpoint) bool {
	return b.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpGet})
}
func (b *mcBootloader) SendSet(n int, e nodelib.Endpoint, d []byte) bool {
	return b.Send(nodelib.Frame{Node: uint8(n), Endpoint: e, Operation: nodelib.OpSet, Data: d})
}

func (b *mcBootloader) reply(ep nodelib.Endpoint, op nodelib.Operation, data []byte) {
	if ep == nodelib.EndpointOtaData && b.latency > 0 {
		// The reply crosses a link with a round trip: it arrives later, so the
		// driver's window can actually fill. (Called with b.mu held.)
		data = append([]byte(nil), data...)
		time.AfterFunc(b.latency, func() {
			b.svc.OnOtaFrame(nodelib.Frame{Node: 0, Endpoint: ep, Operation: op, Data: data})
			b.mu.Lock()
			b.inFlight--
			b.mu.Unlock()
		})
		return
	}
	b.svc.OnOtaFrame(nodelib.Frame{Node: 0, Endpoint: ep, Operation: op, Data: data})
	if ep == nodelib.EndpointOtaData {
		b.inFlight--
	}
}

func (b *mcBootloader) Send(f nodelib.Frame) bool {
	b.mu.Lock()
	defer b.mu.Unlock()

	if b.silent {
		return true
	}
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
		b.gets++
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
		if b.state != nodelib.BlReceiving {
			return true // no transfer in progress: dropped silently, like Firmware::OnData
		}
		b.dataFrames++
		b.inFlight++
		if b.inFlight > b.maxFlight {
			b.maxFlight = b.inFlight
		}
		if b.dropData > 0 {
			b.inFlight-- // nothing will ever answer this one

			b.dropData--
			return true
		}
		off := int(binary.LittleEndian.Uint16(f.Data[0:]))
		chunk := f.Data[2:]
		p := make([]byte, 5)
		switch {
		case off < b.expected: // duplicate: answered from "flash", never re-programmed
			if b.dropAcks > 0 {
				b.dropAcks--
				b.inFlight--
				return true
			}
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
			b.chunks++
			if b.silentAt > 0 && b.chunks == b.silentAt {
				b.silent = true
				b.inFlight--
				return true
			}
			switch {
			case b.restartAt > 0 && b.chunks == b.restartAt:
				// the MCU restarted before it could answer: nothing survives
				b.state, b.expected, b.received = nodelib.BlIdle, 0, nil
				b.inFlight--
			case b.dropAcks > 0:
				b.dropAcks--
				b.inFlight--
			default:
				b.reply(nodelib.EndpointOtaData, nodelib.OpAck, p)
			}
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

// fastOtaTimers shrinks the per-write wait so a stalled link costs
// milliseconds instead of seconds; the resume probe cadence stays at its real
// one-second tick.
func fastOtaTimers(t *testing.T) {
	t.Helper()
	w, r := otaWriteWait, otaMainResumeWait
	otaWriteWait, otaMainResumeWait = 30*time.Millisecond, 8*time.Second
	t.Cleanup(func() { otaWriteWait, otaMainResumeWait = w, r })
}

func TestMainControllerPushResumesAfterALinkStall(t *testing.T) {
	// A run of chunks goes unanswered through every retry -- the NINA stopped
	// forwarding. The push must not fail: it waits, asks the bootloader where
	// it is, and carries on to a complete, byte-exact image.
	fastOtaTimers(t)
	mc := &mcBootloader{dropData: otaWriteRetries + 1}
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if string(mc.received) != string(mcImage(nodelib.ModuleMainController)) {
		t.Error("image after resume differs from the original")
	}
	if !mc.activated {
		t.Error("never activated")
	}
	if mc.gets == 0 {
		t.Error("resume never asked the bootloader for its status")
	}
}

func TestMainControllerPushResumesPastAChunkWhoseAcksWereAllLost(t *testing.T) {
	// The chunk landed but every ack was lost: the bootloader is already past
	// it, so the resume must continue after it, not resend it.
	fastOtaTimers(t)
	mc := &mcBootloader{dropAcks: otaWriteRetries + 1}
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if string(mc.received) != string(mcImage(nodelib.ModuleMainController)) {
		t.Errorf("image after resume differs (%d bytes received)", len(mc.received))
	}
}

func TestMainControllerPushStartsOverWhenTheBootloaderLostItsState(t *testing.T) {
	// The MCU itself restarted mid-transfer: the bootloader is back in Idle
	// with nothing received. The push begins again and still completes.
	fastOtaTimers(t)
	mc := &mcBootloader{restartAt: 3}
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if string(mc.received) != string(mcImage(nodelib.ModuleMainController)) {
		t.Errorf("image after restart differs (%d bytes received)", len(mc.received))
	}
}

func TestMainControllerPushFailsWhenTheLinkNeverComesBack(t *testing.T) {
	fastOtaTimers(t)
	otaMainResumeWait = 1500 * time.Millisecond
	mc := &mcBootloader{silentAt: 2}
	svc, _ := newTestService(t)
	mc.svc = svc
	svc.SetSender(mc)
	id, err := svc.StartOTA(context.Background(), 0, "node", "MainController_3.1.bin", mcImage(nodelib.ModuleMainController), t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		job, err := svc.Store().OtaJob(context.Background(), id)
		if err != nil {
			t.Fatal(err)
		}
		if job.State == "error" {
			if !strings.Contains(job.Error, "did not come back") {
				t.Fatalf("error = %q, want the link-loss message", job.Error)
			}
			return
		}
		if job.State == "done" {
			t.Fatal("push reported done over a dead link")
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("push never gave up on a dead link")
}

func TestMainControllerPushKeepsSeveralChunksInFlight(t *testing.T) {
	// With a round trip on the link the window must actually fill: several
	// requests are out before the first ack is back, never more than the window.
	fastOtaTimers(t)
	otaWriteWait = 2 * time.Second
	mc := &mcBootloader{latency: 20 * time.Millisecond}
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if mc.maxFlight < 2 {
		t.Errorf("never more than %d request in flight -- the window did not fill", mc.maxFlight)
	}
	if mc.maxFlight > otaMainWindow {
		t.Errorf("%d requests in flight, window is %d", mc.maxFlight, otaMainWindow)
	}
	if string(mc.received) != string(mcImage(nodelib.ModuleMainController)) {
		t.Error("image differs")
	}
}

func TestMainControllerPushRecoversAChunkLostInsideTheWindow(t *testing.T) {
	// One request vanishes with others already sent behind it: those are Nacked
	// with the offset the bootloader is at, the sender restarts there, and the
	// image still arrives complete -- without a resend storm.
	fastOtaTimers(t)
	otaWriteWait = 2 * time.Second
	mc := &mcBootloader{latency: 20 * time.Millisecond, dropData: 1}
	// let the loss land mid-transfer, not on the first chunk
	mc.chunks = 0
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if string(mc.received) != string(mcImage(nodelib.ModuleMainController)) {
		t.Error("image differs")
	}
	// 10 chunks; one loss costs at most a window's worth of resends.
	if mc.dataFrames > 10+2*otaMainWindow {
		t.Errorf("%d OtaData requests for 10 chunks: resend storm", mc.dataFrames)
	}
}

func TestMainControllerPushAcceptsALaterAckForALostOne(t *testing.T) {
	// One ack is lost but the chunk landed: the next ack (for a later chunk)
	// proves it, so nothing is resent and no timeout is waited out.
	fastOtaTimers(t)
	otaWriteWait = 5 * time.Second
	mc := &mcBootloader{latency: 20 * time.Millisecond, dropAcks: 1}
	began := time.Now()
	state, msg := runMCPush(t, mc)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	if time.Since(began) > 4*time.Second {
		t.Errorf("took %v: a lost ack was waited out instead of covered by a later one", time.Since(began))
	}
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if mc.dataFrames != 10 {
		t.Errorf("%d OtaData requests for 10 chunks, want exactly 10 (nothing resent)", mc.dataFrames)
	}
}

func TestAwaitWriteReplySkipsAStaleAck(t *testing.T) {
	// A late ack for an earlier chunk (a resend was answered too, or a burst
	// arrived after a stall) must not be taken for the current write.
	d := &otaDriver{writeReplies: make(chan nodelib.FirmwareWriteReply, 4)}
	d.writeReplies <- nodelib.FirmwareWriteReply{Offset: 0, ChunkCRC16: 0x1111}
	d.writeReplies <- nodelib.FirmwareWriteReply{Offset: 32, ChunkCRC16: 0x2222}
	r, ok := d.awaitWriteReply(32, time.Second)
	if !ok || r.Offset != 32 || r.ChunkCRC16 != 0x2222 {
		t.Fatalf("got %+v ok=%v, want the ack for offset 32", r, ok)
	}
	// A Nack is always for the current write: its offset says where to resync.
	d.writeReplies <- nodelib.FirmwareWriteReply{Nack: true, Offset: 64}
	if r, ok := d.awaitWriteReply(96, time.Second); !ok || !r.Nack || r.Offset != 64 {
		t.Fatalf("got %+v ok=%v, want the nack", r, ok)
	}
	if _, ok := d.awaitWriteReply(96, 50*time.Millisecond); ok {
		t.Fatal("got a reply from an empty channel")
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
