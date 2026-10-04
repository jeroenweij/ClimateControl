package service

import (
	"context"
	"encoding/binary"
	"strings"
	"sync"
	"testing"
	"time"

	"climatecontrol/webserver/internal/nodelib"
)

// busNode is a byte-level model of a bus node's bootloader
// (Software/Modules/Bootloader/FirmwareSlave.cpp) behind the Sender: it takes
// the Firmware frames the driver puts on the uplink (as relayed by the
// MainController) and answers as the real one does. Its application does not
// answer anything but EnterBootloader.
type busNode struct {
	svc *Service
	id  int

	mu        sync.Mutex
	app       bool // running the application, not the bootloader
	state     uint8
	expected  int
	size      int
	received  []byte
	activated bool
	chunks    int
	gets      int

	dropData  int  // swallow this many Write frames unanswered
	dropAcks  int  // apply this many Write frames but lose their acks
	restartAt int  // after this many chunks the bootloader restarts with no transfer
	toAppAt   int  // after this many chunks the node boots its application
	silentAt  int  // after this many chunks nothing is answered at all
	silent    bool // the link is gone
}

func (n *busNode) Connected() bool { return true }
func (n *busNode) SendGet(node int, e nodelib.Endpoint) bool {
	return n.Send(nodelib.Frame{Node: uint8(node), Endpoint: e, Operation: nodelib.OpGet})
}
func (n *busNode) SendSet(node int, e nodelib.Endpoint, d []byte) bool {
	return n.Send(nodelib.Frame{Node: uint8(node), Endpoint: e, Operation: nodelib.OpSet, Data: d})
}

func (n *busNode) reply(op nodelib.Operation, data []byte) {
	n.svc.OnNodeFrame(nodelib.Frame{Node: uint8(n.id), Endpoint: nodelib.EndpointFirmware, Operation: op, Data: data})
}

func (n *busNode) status() []byte {
	p := make([]byte, 9)
	p[0] = byte(nodelib.FirmwareOpStatus)
	p[1] = n.state
	binary.LittleEndian.PutUint32(p[2:], uint32(n.expected))
	return p
}

func (n *busNode) Send(f nodelib.Frame) bool {
	n.mu.Lock()
	defer n.mu.Unlock()

	if int(f.Node) != n.id || f.Endpoint != nodelib.EndpointFirmware || n.silent {
		return true
	}
	if n.app {
		if f.Operation == nodelib.OpSet && len(f.Data) > 0 && nodelib.FirmwareOp(f.Data[0]) == nodelib.FirmwareOpEnterBootloader {
			n.app, n.state, n.expected, n.received = false, nodelib.BlIdle, 0, nil
		}
		return true // the application never sends a bootloader status
	}

	switch {
	case f.Operation == nodelib.OpGet:
		n.gets++
		n.reply(nodelib.OpReport, n.status())

	case f.Operation == nodelib.OpSet && len(f.Data) > 0:
		switch nodelib.FirmwareOp(f.Data[0]) {
		case nodelib.FirmwareOpBegin:
			n.size = int(binary.LittleEndian.Uint32(f.Data[2:]))
			n.expected, n.received, n.state = 0, nil, nodelib.BlReceiving
			n.reply(nodelib.OpAck, []byte{0})
		case nodelib.FirmwareOpEnd:
			if n.state != nodelib.BlReceiving || n.expected != n.size {
				n.state = nodelib.BlError
				n.reply(nodelib.OpNack, []byte{nodelib.FwErrBadState})
				return true
			}
			body := n.received[:len(n.received)-4]
			if nodelib.CRC32(body) != binary.LittleEndian.Uint32(n.received[len(n.received)-4:]) {
				n.state = nodelib.BlError
				n.reply(nodelib.OpNack, []byte{nodelib.FwErrCrcMismatch})
				return true
			}
			n.state = nodelib.BlValid
			n.reply(nodelib.OpAck, []byte{0})
		case nodelib.FirmwareOpActivate:
			n.activated = n.state == nodelib.BlValid
		case nodelib.FirmwareOpWrite:
			n.write(f.Data)
		}
	}
	return true
}

func (n *busNode) write(data []byte) {
	if n.state != nodelib.BlReceiving {
		return // no transfer in progress: dropped silently
	}
	if n.dropData > 0 {
		n.dropData--
		return
	}
	off := int(binary.LittleEndian.Uint16(data[1:]))
	chunk := data[3:]
	p := make([]byte, 5)
	switch {
	case off < n.expected: // duplicate: answered from "flash", never re-programmed
		if n.dropAcks > 0 {
			n.dropAcks--
			return
		}
		binary.LittleEndian.PutUint16(p[0:], uint16(off))
		binary.LittleEndian.PutUint16(p[2:], nodelib.CRC16(n.received[off:off+len(chunk)]))
		n.reply(nodelib.OpAck, p)
	case off > n.expected:
		binary.LittleEndian.PutUint16(p[0:], uint16(n.expected))
		n.reply(nodelib.OpNack, p)
	default:
		n.received = append(n.received, chunk...)
		n.expected += len(chunk)
		n.chunks++
		binary.LittleEndian.PutUint16(p[0:], uint16(off))
		binary.LittleEndian.PutUint16(p[2:], nodelib.CRC16(chunk))
		switch {
		case n.restartAt > 0 && n.chunks == n.restartAt:
			n.state, n.expected, n.received = nodelib.BlIdle, 0, nil
		case n.toAppAt > 0 && n.chunks == n.toAppAt:
			n.app, n.state, n.expected, n.received = true, 0, 0, nil
		case n.silentAt > 0 && n.chunks == n.silentAt:
			n.silent = true
		case n.dropAcks > 0:
			n.dropAcks--
		default:
			n.reply(nodelib.OpAck, p)
		}
	}
}

var _ Sender = (*busNode)(nil)

func runNodePush(t *testing.T, n *busNode) (state, errMsg string) {
	t.Helper()
	svc, _ := newTestService(t)
	n.svc, n.id = svc, 1
	svc.SetSender(n)
	id, err := svc.StartOTA(context.Background(), 1, "node", "ControllerNode_3.1.bin", mcImage(nodelib.ModuleControllerNode), t.TempDir())
	if err != nil {
		t.Fatalf("StartOTA: %v", err)
	}
	deadline := time.Now().Add(30 * time.Second)
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

func checkNodeImage(t *testing.T, n *busNode) {
	t.Helper()
	n.mu.Lock()
	defer n.mu.Unlock()
	if string(n.received) != string(mcImage(nodelib.ModuleControllerNode)) {
		t.Errorf("node holds %d bytes that differ from the image", len(n.received))
	}
	if !n.activated {
		t.Error("never activated")
	}
}

func TestNodePushEndToEnd(t *testing.T) {
	fastOtaTimers(t)
	n := &busNode{app: true}
	if state, msg := runNodePush(t, n); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	checkNodeImage(t, n)
}

func TestNodePushResumesAfterALinkStall(t *testing.T) {
	// Every retry of one chunk goes unanswered (the uplink went quiet): the push
	// waits, asks the bootloader where it is and carries on -- it must not fail.
	fastOtaTimers(t)
	n := &busNode{dropData: otaWriteRetries + 1}
	state, msg := runNodePush(t, n)
	if state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	checkNodeImage(t, n)
	if n.gets == 0 {
		t.Error("resume never asked the bootloader for its status")
	}
}

func TestNodePushResumesPastAChunkWhoseAcksWereAllLost(t *testing.T) {
	fastOtaTimers(t)
	n := &busNode{dropAcks: otaWriteRetries + 1}
	if state, msg := runNodePush(t, n); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	checkNodeImage(t, n)
}

func TestNodePushStartsOverWhenTheBootloaderLostItsTransfer(t *testing.T) {
	fastOtaTimers(t)
	n := &busNode{restartAt: 3}
	if state, msg := runNodePush(t, n); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	checkNodeImage(t, n)
}

func TestNodePushReentersTheBootloaderWhenTheNodeBootedItsApp(t *testing.T) {
	// The node restarted and its application started: it answers no status.
	// After a few unanswered probes it is sent back into the bootloader and the
	// image starts over.
	fastOtaTimers(t)
	old := otaNodeReenterAfter
	otaNodeReenterAfter = 2
	t.Cleanup(func() { otaNodeReenterAfter = old })
	n := &busNode{toAppAt: 3}
	if state, msg := runNodePush(t, n); state != "done" {
		t.Fatalf("state = %q (%s), want done", state, msg)
	}
	checkNodeImage(t, n)
}

func TestNodePushFailsWhenTheNodeNeverAnswersAgain(t *testing.T) {
	fastOtaTimers(t)
	otaResumeWait = 1500 * time.Millisecond
	n := &busNode{silentAt: 2}
	state, msg := runNodePush(t, n)
	if state != "error" || !strings.Contains(msg, "did not answer") {
		t.Fatalf("state = %q msg = %q, want an error saying the node did not answer", state, msg)
	}
}

// relayCN stands in for a ControllerNode relaying a Thermostat push. It
// ignores the first `offline` Begins (the node is not on the bus -- still
// rebooting after its own update) and answers the next with the Thermostat's
// bootloader receiving; after that it stays quiet.
type relayCN struct {
	svc     *Service
	mu      sync.Mutex
	offline int
	begins  int
}

func (c *relayCN) Connected() bool { return true }
func (c *relayCN) SendGet(node int, e nodelib.Endpoint) bool {
	return c.Send(nodelib.Frame{Node: uint8(node), Endpoint: e, Operation: nodelib.OpGet})
}
func (c *relayCN) SendSet(node int, e nodelib.Endpoint, d []byte) bool {
	return c.Send(nodelib.Frame{Node: uint8(node), Endpoint: e, Operation: nodelib.OpSet, Data: d})
}
func (c *relayCN) Send(f nodelib.Frame) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if f.Endpoint != nodelib.EndpointThermostatFirmware || f.Operation != nodelib.OpSet ||
		len(f.Data) == 0 || nodelib.FirmwareOp(f.Data[0]) != nodelib.FirmwareOpBegin {
		return true
	}
	c.begins++
	if c.begins <= c.offline {
		return true // went nowhere
	}
	st := make([]byte, 9)
	st[0] = byte(nodelib.FirmwareOpStatus)
	st[1] = nodelib.BlReceiving
	go c.svc.OnNodeFrame(nodelib.Frame{Node: f.Node, Endpoint: nodelib.EndpointThermostatFirmware, Operation: nodelib.OpReport, Data: st})
	return true
}

func TestThermostatPushResendsBeginToAControllerNodeThatWasNotThere(t *testing.T) {
	fastOtaTimers(t)
	step := otaStepWait
	otaStepWait = 150 * time.Millisecond
	t.Cleanup(func() { otaStepWait = step })

	svc, _ := newTestService(t)
	cn := &relayCN{svc: svc, offline: 1}
	svc.SetSender(cn)
	id, err := svc.StartOTA(context.Background(), 3, "thermostat", "Thermostat_3.1.bin", mcImage(nodelib.ModuleThermostat), t.TempDir())
	if err != nil {
		t.Fatalf("StartOTA: %v", err)
	}

	// The first Begin is lost; the resend reaches it and the push moves on to
	// writing (where this fake stops answering -- that later failure is not
	// what is under test).
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		job, err := svc.Store().OtaJob(context.Background(), id)
		if err != nil {
			t.Fatalf("OtaJob: %v", err)
		}
		if job.State == "done" || job.State == "error" {
			if strings.Contains(job.Error, "did not enter bootloader") {
				t.Fatalf("push gave up at Begin: %q", job.Error)
			}
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	cn.mu.Lock()
	defer cn.mu.Unlock()
	if cn.begins < 2 {
		t.Fatalf("Begin sent %d time(s), want a resend", cn.begins)
	}
}
