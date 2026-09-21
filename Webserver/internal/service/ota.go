package service

import (
	"context"
	"os"
	"sync"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/hub"
	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	"github.com/jweij/climatecontrol/webserver/internal/store"
)

const (
	otaChunk = 32 // bytes per Write frame -- Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1 (v2)

	otaReportWait = 3 * time.Second  // per-report timeout (a "thermostat" push's Begin/End/Abort -- still Report/Poll-based)
	otaStepWait   = 20 * time.Second // per-phase timeout
	otaBootPoll   = 1 * time.Second  // Firmware Get cadence while waiting for the bootloader

	// Per-write ack timeout. Write's reply is now a queued Ack/Nack tied to
	// the specific offset (§6.2.1), not a raw progress counter -- but
	// delivery still rides the normal Poll cadence, same as a Report, so
	// this stays close to otaReportWait rather than assuming a much shorter
	// bound. A lost ack or a genuine gap is no longer ambiguous (the reply
	// names its own offset), so a retry can safely resend the actual Write
	// -- FirmwareSlave answers a duplicate from a flash read-back instead of
	// re-programming it (STM32G0 PROGERR on a second write to an
	// already-programmed double-word), so this is safe even if the
	// original write landed and only its ack was lost.
	otaWriteWait    = 3 * time.Second
	otaWriteRetries = 3

	// Found live via the Write/Ack passthrough trace on MainController
	// (OTA-Debugging-TODO.md): a Send() failure (uplink down / outbound
	// queue full) was silently discarded and the driver still waited the
	// full otaWriteWait for a reply that could never arrive. This is the
	// pause before retrying a send in that case instead -- short, since
	// there's nothing to wait out, just a moment for the condition to clear.
	otaSendRetryDelay = 200 * time.Millisecond
)

// StartOTA validates the uploaded image and records a job. Only one push runs
// at a time; every other job waits in the queue (state "queued") and the driver
// picks the next one up as soon as the current push finishes. Both a single
// operator press and an "update all of this type" fan-out land here.
//
// target is "node" (flash the bus node itself) or "thermostat" (flash the
// Thermostat paired to the ControllerNode at nodeID, relayed over its private
// link — ControllerNode-Thermostat-Link-Spec.md §5). For a thermostat push the
// image's descriptor module must be Thermostat.
func (s *Service) StartOTA(ctx context.Context, nodeID int, target, filename string, bin []byte, imageDir string) (int64, error) {
	if target == "" {
		target = "node"
	}
	if target != "node" && target != "thermostat" {
		return 0, ErrOtaTargetMismatch
	}

	desc, crc, err := nodelib.ParseImage(bin)
	if err != nil {
		return 0, err
	}

	isThermImage := desc.Module == nodelib.ModuleThermostat
	if (target == "thermostat") != isThermImage {
		return 0, ErrOtaTargetMismatch
	}
	pushModule := desc.Module
	if target == "thermostat" {
		pushModule = nodelib.ModuleThermostat
	}

	// One press = one job: an already-queued or running job for the same
	// node+target absorbs the repeat instead of stacking a duplicate.
	if pending, err := s.st.HasPendingOtaJob(ctx, nodeID, target); err != nil {
		return 0, err
	} else if pending {
		return 0, ErrOtaQueued
	}

	if err := os.MkdirAll(imageDir, 0o755); err != nil {
		return 0, err
	}
	path := imageDir + "/ota-" + time.Now().Format("20060102-150405.000") + "-" + filename
	if err := os.WriteFile(path, bin, 0o644); err != nil {
		return 0, err
	}

	jobID, err := s.st.CreateOtaJob(ctx, store.OtaJob{
		NodeID:    nodeID,
		Target:    target,
		Filename:  filename,
		Size:      len(bin),
		CRC32:     crc,
		FWVersion: int(desc.FWVersionMajor)<<8 | int(desc.FWVersionMinor),
		Module:    int(pushModule),
		ImagePath: path,
	})
	if err != nil {
		return 0, err
	}
	s.hb.PublishOta(hub.OtaEvent{JobID: jobID, State: "queued", NodeID: nodeID, Size: len(bin)})

	s.kickOta()
	return jobID, nil
}

// kickOta starts the next queued push if nothing is running. It is safe to call
// from anywhere: after enqueue, when a push finishes, and on uplink reconnect.
func (s *Service) kickOta() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ota != nil && !s.ota.finished() {
		return
	}
	if !s.send.Connected() {
		return // retry on the next kick (OnConnect / next enqueue)
	}
	ctx := context.Background()
	for {
		job, ok, err := s.st.NextQueuedOtaJob(ctx)
		if err != nil || !ok {
			s.ota = nil
			return
		}
		bin, err := os.ReadFile(job.ImagePath)
		if err != nil {
			_ = s.st.UpdateOtaJob(ctx, job.ID, "error", 0, "image file missing: "+err.Error())
			s.hb.PublishOta(hub.OtaEvent{JobID: job.ID, State: "error", NodeID: job.NodeID, Error: "image file missing"})
			continue
		}
		d := &otaDriver{
			svc:          s,
			jobID:        job.ID,
			nodeID:       job.NodeID,
			target:       job.Target,
			image:        bin,
			crc32:        job.CRC32,
			fw:           uint16(job.FWVersion),
			module:       nodelib.Module(job.Module),
			reports:      make(chan nodelib.FirmwareStatusReport, 8),
			writeReplies: make(chan nodelib.FirmwareWriteReply, 8),
			opReplies:    make(chan nodelib.FirmwareOpReply, 8),
			doneCh:       make(chan struct{}),
		}
		s.ota = d
		go d.run()
		return
	}
}

// otaDriver drives one push directly over the bus's ordinary Endpoint::Firmware
// / Endpoint::ThermostatFirmware sequence -- both are plain relayed endpoints
// (block 0x10-0x50), so this needs nothing from the MainController beyond the
// generic relay it already does for every other endpoint. See the "why not a
// wrapper endpoint" discussion this replaces: MainController-Server-Link-
// Spec.md §8 used to route this through a 0x65/0x66 uplink-only pair that the
// MC had to rewrite into bus Firmware frames; the MC never actually needed
// that translation for a plain node push (Firmware already relays), so it's
// gone. targetNodeId == 0 (MainController self-update) still isn't handled --
// Open item, needs a RAM-resident self-flash routine, not a relay at all.
type otaDriver struct {
	svc    *Service
	jobID  int64
	nodeID int
	target string // "node" | "thermostat"
	image  []byte
	crc32  uint32
	fw     uint16
	module nodelib.Module

	reports      chan nodelib.FirmwareStatusReport
	writeReplies chan nodelib.FirmwareWriteReply
	opReplies    chan nodelib.FirmwareOpReply
	doneCh       chan struct{}
	once         sync.Once
}

// endpoint is Endpoint::Firmware for a "node" push, or Endpoint::
// ThermostatFirmware -- addressed to the *owning ControllerNode's* node id --
// for a "thermostat" push (ControllerNode-Thermostat-Link-Spec.md §5.4).
func (d *otaDriver) endpoint() nodelib.Endpoint {
	if d.target == "thermostat" {
		return nodelib.EndpointThermostatFirmware
	}
	return nodelib.EndpointFirmware
}

// sendSet returns whether the frame was actually queued for the uplink --
// false means it never left the server at all (uplink down, or the outbound
// queue was full), which previously burned a full otaWriteWait doing nothing
// (Spec/OTA-Debugging-TODO.md: a live Write/Ack passthrough trace on
// MainController showed one of these events directly -- the MC never even
// saw a Write to relay for the chunk that finally timed out).
func (d *otaDriver) sendSet(data []byte) bool {
	ok := d.svc.send.Send(nodelib.Frame{Node: uint8(d.nodeID), Endpoint: d.endpoint(), Operation: nodelib.OpSet, Data: data})
	if !ok {
		d.svc.log.Warn("ota: send failed, not queued for uplink", "job", d.jobID, "node", d.nodeID, "target", d.target)
	}
	return ok
}

// onReport is called by Service.onFirmwareReport for every relayed Firmware /
// ThermostatFirmware report; it filters to this job's node+endpoint and feeds
// the decoded Status onto the channel run() reads from.
func (d *otaDriver) onReport(f nodelib.Frame) {
	if int(f.Node) != d.nodeID || f.Endpoint != d.endpoint() {
		return
	}
	r, ok := nodelib.ParseFirmwareStatusReport(f.Data)
	if !ok {
		return
	}
	select {
	case d.reports <- r:
	default:
	}
}

// onWriteReply is called by Service.onFirmwareWriteReply for every relayed
// Firmware / ThermostatFirmware Ack/Nack; it filters to this job's
// node+endpoint and feeds the decoded reply onto whichever channel run()
// reads it from. Begin/End/Abort's 1-byte lastError-only reply and Write's
// 5-byte offset/CRC/programFailed reply share the same Operation, so the
// payload length is what tells them apart (see FirmwareOpReply /
// FirmwareWriteReply).
func (d *otaDriver) onWriteReply(nack bool, f nodelib.Frame) {
	if int(f.Node) != d.nodeID || f.Endpoint != d.endpoint() {
		return
	}
	if r, ok := nodelib.ParseFirmwareOpReply(nack, f.Data); ok {
		select {
		case d.opReplies <- r:
		default:
		}
		return
	}
	r, ok := nodelib.ParseFirmwareWriteReply(nack, f.Data)
	if !ok {
		return
	}
	select {
	case d.writeReplies <- r:
	default:
	}
}

func (d *otaDriver) finished() bool {
	select {
	case <-d.doneCh:
		return true
	default:
		return false
	}
}

func (d *otaDriver) done(state, errMsg string, offset int) {
	d.once.Do(func() {
		_ = d.svc.st.UpdateOtaJob(context.Background(), d.jobID, state, offset, errMsg)
		d.svc.hb.PublishOta(hub.OtaEvent{
			JobID: d.jobID, State: state, NodeID: d.nodeID,
			Offset: offset, Size: len(d.image), Error: errMsg,
		})
		close(d.doneCh)
		// Hand off to the next queued push, if any.
		go d.svc.kickOta()
	})
}

func (d *otaDriver) progress(state string, offset int) {
	_ = d.svc.st.UpdateOtaJob(context.Background(), d.jobID, state, offset, "")
	d.svc.hb.PublishOta(hub.OtaEvent{
		JobID: d.jobID, State: state, NodeID: d.nodeID, Offset: offset, Size: len(d.image),
	})
}

func (d *otaDriver) run() {
	// Cheap sanity pin before touching the bus at all: re-validate the
	// in-memory/on-disk image against the CRC32 recorded at upload time
	// (Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1 "Server-side
	// chunking bug", point 3) -- catches corruption between upload and use,
	// independent of anything about the chunking loop below.
	if _, crc, err := nodelib.ParseImage(d.image); err != nil || crc != d.crc32 {
		d.done("error", "uploaded image failed integrity re-check before starting", 0)
		return
	}

	d.progress("entering bootloader", 0)

	if d.target == "thermostat" {
		// The ControllerNode does EnterBootloader + the bootloader-Announce
		// wait itself (spec §5.4) -- Begin is the whole first step here.
		d.sendSet(nodelib.EncodeThermostatFirmwareBegin(uint32(len(d.image)), d.crc32, d.fw, false))
	} else {
		if !d.enterBootloader() {
			d.done("error", "node did not enter bootloader", 0)
			return
		}
		d.sendSet(nodelib.EncodeFirmwareBegin(d.module, uint32(len(d.image)), d.crc32, d.fw))
	}

	r, ok := d.waitForWithProbe(otaStepWait, otaBootPoll, nodelib.BlReceiving)
	if !ok {
		d.done("error", "node did not enter bootloader / begin", 0)
		return
	}
	if r.LastError == nodelib.FwErrAlreadyCurrent {
		// Thermostat-only guard (§5.4.1): the CN skipped a no-op re-flash.
		d.done("done", "", len(d.image))
		return
	}
	if r.State == nodelib.BlError {
		d.done("error", "node reported error: "+nodelib.FirmwareErrorName(r.LastError), 0)
		return
	}
	d.progress("writing", 0)

	// Stream Write chunks one at a time, each a full request/reply: send,
	// wait for its Ack/Nack, verify the CRC it reports against what we meant
	// to send, then move on. See Node-Flash-Layout-and-Bootloader-Spec.md
	// §6.2.1 for the design and the "Server-side chunking bug" note this
	// implements point 1 of (slicing 'chunk' fresh from 'offset', the exact
	// value written into the wire message, rather than a separately-tracked
	// loop variable that could drift out of sync with it).
	offset := 0
	for offset < len(d.image) {
		end := offset + otaChunk
		if end > len(d.image) {
			end = len(d.image)
		}
		chunk := d.image[offset:end]
		expectCRC := nodelib.CRC16(chunk)
		wireOffset := uint16(offset)

		var reply nodelib.FirmwareWriteReply
		var ok bool
		for attempt := 0; attempt <= otaWriteRetries; attempt++ {
			// A resend is always safe now, not just a probe: FirmwareSlave
			// answers a duplicate offset from a flash read-back rather than
			// re-programming it, so repeating the write when only its ack
			// was lost just gets a fresh Ack for the same, already-correct
			// data.
			if !d.sendSet(nodelib.EncodeFirmwareWrite(wireOffset, chunk)) {
				// Never left the server (uplink down / outbound queue full)
				// -- no reply can possibly come back, so don't burn a full
				// otaWriteWait waiting for one; pause briefly and retry.
				time.Sleep(otaSendRetryDelay)
				continue
			}
			reply, ok = d.awaitWriteReply(otaWriteWait)
			if ok {
				break
			}
			d.svc.log.Warn("ota: write sent, no reply within otaWriteWait", "job", d.jobID, "offset", offset,
				"attempt", attempt, "waitedMs", otaWriteWait.Milliseconds())
		}
		if !ok {
			d.svc.log.Warn("ota: write ack timed out after all retries", "job", d.jobID, "offset", offset,
				"attempts", otaWriteRetries+1)
			d.done("error", "timeout waiting for write ack", offset)
			return
		}
		if reply.Nack {
			// Node named where it actually is (a gap, or "can't vouch past
			// what's committed") -- resync there and retry from that point.
			offset = int(reply.Offset)
			d.progress("writing", offset)
			continue
		}
		if reply.ProgramFailed {
			d.done("error", "node reported a flash program failure", offset)
			return
		}
		if reply.ChunkCRC16 != expectCRC {
			// The node applied different bytes than we sent for this offset
			// -- a receive/logic bug, independent of flash (the frame's own
			// CRC16 already protects the wire hop; this catches corruption
			// downstream of that, or a bug in what the node staged).
			d.done("error", "chunk CRC mismatch -- node staged different bytes than sent", offset)
			return
		}
		offset = end
		d.progress("writing", offset)
	}

	// End marker + verify. Every chunk sent above was individually verified
	// against what this driver meant to send, so if this final whole-image
	// CRC32 still fails, that specific combination points at a server-side
	// chunking bug (wrong bytes sliced for some offset -- both this driver's
	// own per-chunk CRC and the node's would agree on the same wrong bytes,
	// so per-chunk verification alone can't catch it), not the node/flash
	// path -- see Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1 "Server-
	// side chunking bug".
	d.sendSet(nodelib.EncodeFirmwareEnd())
	d.progress("verifying", offset)
	r, ok = d.waitForWithProbe(otaStepWait, otaBootPoll, nodelib.BlValid)
	if !ok {
		d.done("error", "image did not verify on the node", offset)
		return
	}
	if r.State == nodelib.BlError {
		if r.LastError == nodelib.FwErrCrcMismatch {
			d.done("error", "image verified chunk-by-chunk during transfer but failed the final image CRC check "+
				"-- likely a server-side chunking bug, not a node/flash issue", offset)
		} else {
			d.done("error", "node reported error: "+nodelib.FirmwareErrorName(r.LastError), offset)
		}
		return
	}
	d.sendSet(nodelib.EncodeFirmwareActivate())
	d.done("done", "", len(d.image))
}

// enterBootloader sends Firmware[EnterBootloader] then polls Firmware Get
// every otaBootPoll until any Status report arrives (only the bootloader ever
// sends one -- the running app only Acks/Nacks Firmware) or timeout.
func (d *otaDriver) enterBootloader() bool {
	d.sendSet(nodelib.EncodeFirmwareEnterBootloader())

	deadline := time.After(otaStepWait)
	ticker := time.NewTicker(otaBootPoll)
	defer ticker.Stop()
	for {
		select {
		case <-d.reports:
			return true
		case <-ticker.C:
			d.svc.send.SendGet(d.nodeID, nodelib.EndpointFirmware)
		case <-deadline:
			return false
		}
	}
}

// waitForWithProbe is waitFor, but re-sends Firmware[Get] every probeInterval
// while it waits -- a single lost Set, or a single lost reply, is otherwise
// unrecoverable within the timeout (see Node-Flash-Layout-and-Bootloader-
// Spec.md §8 item 9 and OTA-Debugging-TODO.md's job-60 finding -- a clean
// transfer failing only because the End Report or Begin's Set/Report was lost
// once, with no retry budget at all). Get always re-arms statusPending
// regardless of what's actually pending, so it safely re-elicits a fresh
// report whatever step we're waiting on.
//
// A "node" push's Begin/End/Abort now reply directly via Ack/Nack (matching
// what Write already did) instead of the Report this originally probed for
// -- opReplies carries that. It settles the wait immediately rather than
// leaving it to the next probe tick: an Ack only ever gets queued once
// FirmwareSlave has already moved into the state being waited for
// (FirmwareSlave.cpp's HandleBegin/HandleEnd), so it's synthesized as a
// Report claiming exactly that state; a Nack carries the same lastError a
// probed Report would eventually have. A "thermostat" push still only ever
// produces Reports here (ControllerHandler.cpp's own Begin/End/Abort
// handling wasn't part of this migration), so this is purely additive for it.
func (d *otaDriver) waitForWithProbe(timeout, probeInterval time.Duration, state uint8) (nodelib.FirmwareStatusReport, bool) {
	deadline := time.After(timeout)
	ticker := time.NewTicker(probeInterval)
	defer ticker.Stop()
	for {
		select {
		case r := <-d.reports:
			if r.State == state || r.State == nodelib.BlError || r.LastError == nodelib.FwErrAlreadyCurrent {
				return r, true
			}
		case r := <-d.opReplies:
			if r.Nack {
				return nodelib.FirmwareStatusReport{State: nodelib.BlError, LastError: r.LastError}, true
			}
			return nodelib.FirmwareStatusReport{State: state, LastError: nodelib.FwErrNone}, true
		case <-ticker.C:
			d.svc.send.SendGet(d.nodeID, nodelib.EndpointFirmware)
		case <-deadline:
			return nodelib.FirmwareStatusReport{}, false
		}
	}
}

func (d *otaDriver) awaitReport(timeout time.Duration) (nodelib.FirmwareStatusReport, bool) {
	select {
	case r := <-d.reports:
		return r, true
	case <-time.After(timeout):
		return nodelib.FirmwareStatusReport{}, false
	}
}

func (d *otaDriver) awaitWriteReply(timeout time.Duration) (nodelib.FirmwareWriteReply, bool) {
	select {
	case r := <-d.writeReplies:
		return r, true
	case <-time.After(timeout):
		return nodelib.FirmwareWriteReply{}, false
	}
}
