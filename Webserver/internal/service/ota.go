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
	otaWriteRetries = 3

	// How many times one push may lose the link mid-transfer and carry on
	// once it is back (runMainController's resume hook) before giving up.
	otaMaxResumes = 20

	// Found live via the Write/Ack passthrough trace on MainController
	// (OTA-Debugging-TODO.md): a Send() failure (uplink down / outbound
	// queue full) was silently discarded and the driver still waited the
	// full otaWriteWait for a reply that could never arrive. This is the
	// pause before retrying a send in that case instead -- short, since
	// there's nothing to wait out, just a moment for the condition to clear.
	otaSendRetryDelay = 200 * time.Millisecond
)

// otaWriteWait is a var only so tests can shrink it.
var otaWriteWait = 3 * time.Second

// How long a transfer waits for the uplink to come back after it goes quiet
// mid-write: the MainController notices within ~20 s (an unanswered keepalive),
// resets the NINA and reconnects in ~10 s. A var only so tests can shrink it.
var otaResumeWait = 120 * time.Second

// After this many status probes that actually went out, unanswered, a bus
// node is presumed to have restarted into its application (which never sends
// a bootloader status) and is sent back into the bootloader. Generous on
// purpose: probes are counted while the server still thinks the uplink is up,
// which covers the ~20 s the MainController needs to notice a stalled module
// plus a reconnect that can take 25 s -- a node whose bootloader is merely
// waiting behind that must not be restarted from zero.
var otaNodeReenterAfter = 90

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
	return s.startOTA(ctx, nodeID, target, filename, bin, imageDir, false)
}

// startOTA is StartOTA with force: a "thermostat" push then carries the
// Force flag, so the ControllerNode re-flashes even an already-current
// Thermostat (link spec §5.4.1). No effect on a "node" push.
func (s *Service) startOTA(ctx context.Context, nodeID int, target, filename string, bin []byte, imageDir string, force bool) (int64, error) {
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

	// MainController (node 0) is updated through its own bootloader's
	// OtaControl/OtaData protocol, and only its own image is valid for it.
	if target == "node" && (nodeID == 0) != (desc.Module == nodelib.ModuleMainController) {
		return 0, ErrOtaTargetMismatch
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
		Force:     force && target == "thermostat",
	})
	if err != nil {
		return 0, err
	}
	s.hb.PublishOta(hub.OtaEvent{JobID: jobID, State: "queued", NodeID: nodeID, Size: len(bin)})
	s.PruneOta()

	s.kickOta()
	return jobID, nil
}

// PruneOta drops finished push records beyond the newest store.OtaJobsKept and
// deletes the image copy each one kept. Called when a push is queued and when
// one finishes (and once at startup), so the queue never grows without bound.
func (s *Service) PruneOta() {
	paths, err := s.st.PruneOtaJobs(context.Background())
	if err != nil {
		s.log.Warn("ota: prune old jobs", "err", err)
		return
	}
	for _, p := range paths {
		_ = os.Remove(p)
	}
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
			svc:            s,
			jobID:          job.ID,
			nodeID:         job.NodeID,
			target:         job.Target,
			image:          bin,
			crc32:          job.CRC32,
			fw:             uint16(job.FWVersion),
			force:          job.Force,
			module:         nodelib.Module(job.Module),
			mainController: job.NodeID == 0 && job.Target == "node",
			reports:        make(chan nodelib.FirmwareStatusReport, 8),
			writeReplies:   make(chan nodelib.FirmwareWriteReply, 32),
			opReplies:      make(chan nodelib.FirmwareOpReply, 8),
			doneCh:         make(chan struct{}),
		}
		s.ota = d
		go d.run()
		return
	}
}

// otaDriver drives one push. A bus node (or a Thermostat behind a
// ControllerNode) is updated over the ordinary Endpoint::Firmware /
// Endpoint::ThermostatFirmware sequence -- both are plain relayed endpoints
// (block 0x10-0x50), so this needs nothing from the MainController beyond the
// generic relay it already does for every other endpoint. MainController
// itself (node 0) has no relay target: it is parked in its own bootloader and
// updated over the uplink-only OtaControl / OtaData pair (ota_main.go).
type otaDriver struct {
	svc    *Service
	jobID  int64
	nodeID int
	target string // "node" | "thermostat"
	image  []byte
	crc32  uint32
	fw     uint16
	force  bool // thermostat push: Begin with the Force flag (store.OtaJob.Force)
	module nodelib.Module

	// mainController: this push targets the MainController itself (node 0),
	// driven over OtaControl/OtaData by runMainController.
	mainController bool

	// resume, when set, is called if a chunk goes unanswered through every retry:
	// it waits for the link to come back and returns the offset to carry on
	// from. Nil means such a stall fails the push.
	resume func(offset int) (int, bool)

	// window is how many chunks may be in flight at once; 0 or 1 is plain
	// stop-and-wait. Only the MainController push sets it (see runMainController).
	window int

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
	return d.sendFrame(d.endpoint(), nodelib.OpSet, data)
}

func (d *otaDriver) sendFrame(ep nodelib.Endpoint, op nodelib.Operation, data []byte) bool {
	ok := d.svc.send.Send(nodelib.Frame{Node: uint8(d.nodeID), Endpoint: ep, Operation: op, Data: data})
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

// onOtaFrame is called by Service.OnOtaFrame for every OtaControl / OtaData
// frame from MainController's bootloader; only a MainController push consumes
// them. OtaControl Report is the status answer to a Get, OtaControl Ack/Nack
// answers Begin/End/Abort, and OtaData Ack/Nack answers one write chunk.
func (d *otaDriver) onOtaFrame(f nodelib.Frame) {
	if !d.mainController {
		return
	}
	switch {
	case f.Endpoint == nodelib.EndpointOtaControl && f.Operation == nodelib.OpReport:
		if r, ok := nodelib.ParseOtaStatusReport(f.Data); ok {
			select {
			case d.reports <- r:
			default:
			}
		}
	case f.Endpoint == nodelib.EndpointOtaControl && (f.Operation == nodelib.OpAck || f.Operation == nodelib.OpNack):
		if r, ok := nodelib.ParseFirmwareOpReply(f.Operation == nodelib.OpNack, f.Data); ok {
			select {
			case d.opReplies <- r:
			default:
			}
		}
	case f.Endpoint == nodelib.EndpointOtaData && (f.Operation == nodelib.OpAck || f.Operation == nodelib.OpNack):
		if r, ok := nodelib.ParseFirmwareWriteReply(f.Operation == nodelib.OpNack, f.Data); ok {
			select {
			case d.writeReplies <- r:
			default:
			}
		}
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
		d.svc.PruneOta()
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

	if d.mainController {
		d.runMainController()
		return
	}

	d.progress("entering bootloader", 0)

	if d.target == "thermostat" {
		// The ControllerNode does EnterBootloader + the bootloader-Announce
		// wait itself (spec §5.4) -- Begin is the whole first step here.
		d.sendSet(nodelib.EncodeThermostatFirmwareBegin(uint32(len(d.image)), d.crc32, d.fw, d.force))
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

	d.resume = d.resumeNode
	offset, ok := d.writeImage(d.endpoint(), nodelib.EncodeFirmwareWrite)
	if !ok {
		return
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

// writeImage streams the image as one write frame per chunk on endpoint ep,
// each a full request/reply: send, wait for its Ack/Nack, verify the CRC it
// reports against what we meant to send, then move on. encode builds the
// endpoint's write payload (bus Firmware[Write] or OtaData). It returns the
// offset reached; ok is false if the push failed (done() has already been
// called with the reason).
//
// See Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1 for the design and the
// "Server-side chunking bug" note this implements point 1 of (slicing 'chunk'
// fresh from 'offset', the exact value written into the wire message, rather
// than a separately-tracked loop variable that could drift out of sync with
// it).
func (d *otaDriver) writeImage(ep nodelib.Endpoint, encode func(offset uint16, chunk []byte) []byte) (int, bool) {
	if d.window > 1 {
		return d.writeImageWindowed(ep, encode)
	}
	offset := 0

	// Per-chunk round-trip timing (send -> Ack), logged once at the end: the
	// transfer is stop-and-wait, so total time is chunks x (serial + flash +
	// link RTT) and this shows which term dominates.
	began := time.Now()
	var chunks, retries int
	var rttSum, rttMax time.Duration
	logSummary := func() {
		if chunks == 0 {
			return
		}
		elapsed := time.Since(began)
		d.svc.log.Info("ota: image write finished", "job", d.jobID, "bytes", offset, "chunks", chunks,
			"retries", retries, "elapsed", elapsed.Round(time.Millisecond),
			"avgRTT", (rttSum / time.Duration(chunks)).Round(100*time.Microsecond),
			"maxRTT", rttMax.Round(100*time.Microsecond),
			"bytesPerSec", int(float64(offset)/elapsed.Seconds()))
	}
	defer logSummary()

	resumes := 0
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
		var sentAt time.Time
		for attempt := 0; attempt <= otaWriteRetries; attempt++ {
			if attempt > 0 {
				retries++
			}
			// A resend is always safe: the receiver answers a duplicate offset
			// from a flash read-back rather than re-programming it, so
			// repeating the write when only its ack was lost just gets a fresh
			// Ack for the same, already-correct data.
			if !d.sendFrame(ep, nodelib.OpSet, encode(wireOffset, chunk)) {
				// Never left the server (uplink down / outbound queue full)
				// -- no reply can possibly come back, so don't burn a full
				// otaWriteWait waiting for one; pause briefly and retry.
				time.Sleep(otaSendRetryDelay)
				continue
			}
			sentAt = time.Now()
			reply, ok = d.awaitWriteReply(wireOffset, otaWriteWait)
			if ok {
				break
			}
			d.svc.log.Warn("ota: write sent, no reply within otaWriteWait", "job", d.jobID, "offset", offset,
				"attempt", attempt, "waitedMs", otaWriteWait.Milliseconds())
		}
		if !ok {
			d.svc.log.Warn("ota: write ack timed out after all retries", "job", d.jobID, "offset", offset,
				"attempts", otaWriteRetries+1)
			if d.resume != nil && resumes < otaMaxResumes {
				// The link went quiet (the module can stall for tens of
				// seconds; the MainController then resets it and reconnects).
				// The bootloader keeps what it has received, so wait for it
				// and carry on from wherever it says it is.
				resumes++
				next, resumed := d.resume(offset)
				if !resumed {
					return offset, false // resume() has already ended the job
				}
				d.svc.log.Info("ota: resumed after link loss", "job", d.jobID, "from", offset, "to", next, "resumes", resumes)
				offset = next
				continue
			}
			d.done("error", "timeout waiting for write ack", offset)
			return offset, false
		}
		if rtt := time.Since(sentAt); ok {
			chunks++
			rttSum += rtt
			if rtt > rttMax {
				rttMax = rtt
			}
		}
		if reply.Nack {
			// Receiver named where it actually is (a gap, or "can't vouch
			// past what's committed") -- resync there and retry from that
			// point.
			offset = int(reply.Offset)
			d.progress("writing", offset)
			continue
		}
		if reply.ProgramFailed {
			d.done("error", "node reported a flash program failure", offset)
			return offset, false
		}
		if reply.ChunkCRC16 != expectCRC {
			// The receiver applied different bytes than we sent for this
			// offset -- a receive/logic bug, independent of flash (the
			// frame's own CRC16 already protects the wire hop; this catches
			// corruption downstream of that, or a bug in what it staged).
			d.done("error", "chunk CRC mismatch -- node staged different bytes than sent", offset)
			return offset, false
		}
		offset = end
		d.progress("writing", offset)
	}
	return offset, true
}

// resumeNode is writeImage's answer, for a bus node (or the Thermostat behind
// a ControllerNode), to a chunk that went unanswered through every retry. The
// usual cause is the uplink going quiet: the MainController resets its NINA
// and reconnects, while the node -- on the bus, untouched -- keeps everything
// it has received. So wait for the link, ask the node's bootloader where it is
// and carry on from the offset it names. A bootloader with no transfer (it
// restarted) is sent Begin again and the image starts over; a node that never
// answers is presumed to have booted its application and is sent back into
// the bootloader first. Returns the offset to continue from; when it returns
// false the job has already been ended.
func (d *otaDriver) resumeNode(offset int) (int, bool) {
	d.progress("reconnecting", offset)
	d.drainReplies()

	deadline := time.After(otaResumeWait)
	ticker := time.NewTicker(otaBootPoll)
	defer ticker.Stop()
	probes := 0
	for {
		select {
		case r := <-d.reports:
			probes = 0
			switch r.State {
			case nodelib.BlReceiving:
				next := int(r.ExpectedOffset)
				if next > len(d.image) {
					d.done("error", "node reports an impossible resume offset", offset)
					return 0, false
				}
				d.drainReplies()
				d.progress("writing", next)
				return next, true
			case nodelib.BlError:
				d.done("error", "node reported error: "+nodelib.FirmwareErrorName(r.LastError), offset)
				return 0, false
			default:
				return d.restartNodePush()
			}
		case <-ticker.C:
			if !d.svc.send.SendGet(d.nodeID, d.endpoint()) {
				continue // link down: nothing went out, so nothing to wait for
			}
			probes++
			if probes >= otaNodeReenterAfter && d.target != "thermostat" {
				probes = 0
				d.progress("entering bootloader", 0)
				if !d.enterBootloader() {
					d.done("error", "node did not answer after the link was lost", offset)
					return 0, false
				}
				return d.restartNodePush()
			}
		case <-deadline:
			d.done("error", "node did not answer after the link was lost", offset)
			return 0, false
		}
	}
}

// restartNodePush sends Begin again to a bootloader that holds no transfer
// and waits for it to start receiving; the image then goes from offset 0.
func (d *otaDriver) restartNodePush() (int, bool) {
	d.progress("erasing", 0)
	if d.target == "thermostat" {
		// Force: the ControllerNode must not skip this as an already-current no-op.
		d.sendSet(nodelib.EncodeThermostatFirmwareBegin(uint32(len(d.image)), d.crc32, d.fw, true))
	} else {
		d.sendSet(nodelib.EncodeFirmwareBegin(d.module, uint32(len(d.image)), d.crc32, d.fw))
	}
	r, ok := d.waitForWithProbe(otaStepWait, otaBootPoll, nodelib.BlReceiving)
	if !ok {
		d.done("error", "node did not enter bootloader / begin", 0)
		return 0, false
	}
	if r.State == nodelib.BlError {
		d.done("error", "node reported error: "+nodelib.FirmwareErrorName(r.LastError), 0)
		return 0, false
	}
	d.drainReplies()
	d.progress("writing", 0)
	return 0, true
}

// writeImageWindowed is writeImage with several chunks in flight (go-back-N):
// requests go out back to back and each ack slides the window, so the link
// carries a steady stream instead of one request per round trip. That is
// faster, and it matters for the NINA module too: it can stop forwarding the
// MainController's bytes when traffic across it goes quiet for ~120 ms, which
// is exactly what one-chunk-per-round-trip does.
//
// The receiver (Boot::Firmware) takes chunks strictly in order: a chunk past
// its expected offset is Nacked with that offset, a chunk it already has is
// acked again from flash. So:
//   - an ack for offset X means every chunk up to and including X landed,
//     even if the acks before it were lost;
//   - a Nack names where to restart, and one loss produces a Nack per chunk
//     sent behind it, so repeats of the same Nack are ignored for a moment;
//   - no progress for otaWriteWait means resend from the oldest unacked chunk.
func (d *otaDriver) writeImageWindowed(ep nodelib.Endpoint, encode func(offset uint16, chunk []byte) []byte) (int, bool) {
	chunkEnd := func(off int) int {
		if end := off + otaChunk; end < len(d.image) {
			return end
		}
		return len(d.image)
	}

	began := time.Now()
	var acked, retries int
	var rttSum, rttMax time.Duration
	base, next := 0, 0 // base: first unacked byte; next: next byte to send
	sentAt := map[int]time.Time{}
	lastProgress := time.Now()
	timeouts, resumes := 0, 0
	var nackHoldUntil time.Time

	defer func() {
		if acked == 0 {
			return
		}
		elapsed := time.Since(began)
		d.svc.log.Info("ota: image write finished", "job", d.jobID, "bytes", base, "chunks", acked,
			"window", d.window, "retries", retries, "elapsed", elapsed.Round(time.Millisecond),
			"avgRTT", (rttSum / time.Duration(acked)).Round(100*time.Microsecond),
			"maxRTT", rttMax.Round(100*time.Microsecond),
			"bytesPerSec", int(float64(base)/elapsed.Seconds()))
	}()

	restart := func(at int) {
		base, next = at, at
		for k := range sentAt {
			delete(sentAt, k)
		}
		lastProgress = time.Now()
		timeouts = 0
	}

	for base < len(d.image) {
		for next < len(d.image) && next-base < d.window*otaChunk {
			if !d.sendFrame(ep, nodelib.OpSet, encode(uint16(next), d.image[next:chunkEnd(next)])) {
				// Never left the server (uplink down / queue full): nothing can
				// come back for it; the stall deadline below bounds the wait.
				time.Sleep(otaSendRetryDelay)
				break
			}
			sentAt[next] = time.Now()
			next = chunkEnd(next)
		}

		wait := time.Until(lastProgress.Add(otaWriteWait))
		if wait > 0 {
			select {
			case r := <-d.writeReplies:
				if r.ProgramFailed {
					d.done("error", "node reported a flash program failure", base)
					return base, false
				}
				if r.Nack {
					at := int(r.Offset)
					if at > len(d.image) || (at == base && time.Now().Before(nackHoldUntil)) {
						continue // malformed, or a repeat of the Nack already acted on
					}
					nackHoldUntil = time.Now().Add(otaWriteWait / 2)
					next = at
					base = at
					for k := range sentAt {
						delete(sentAt, k)
					}
					d.progress("writing", base)
					continue
				}
				off := int(r.Offset)
				if off < base || off >= next {
					continue // late ack for something already settled, or never sent
				}
				end := chunkEnd(off)
				if r.ChunkCRC16 != nodelib.CRC16(d.image[off:end]) {
					// The receiver applied different bytes than we sent for this
					// offset -- a receive/logic bug, independent of flash.
					d.done("error", "chunk CRC mismatch -- node staged different bytes than sent", off)
					return base, false
				}
				if t, ok := sentAt[off]; ok {
					rtt := time.Since(t)
					rttSum += rtt
					if rtt > rttMax {
						rttMax = rtt
					}
				}
				acked += (end - base + otaChunk - 1) / otaChunk
				for k := range sentAt {
					if k < end {
						delete(sentAt, k)
					}
				}
				base = end
				lastProgress = time.Now()
				timeouts = 0
				d.progress("writing", base)
			case <-time.After(wait):
			}
			continue
		}

		// No progress for a full otaWriteWait: resend from the oldest unacked chunk.
		timeouts++
		retries++
		d.svc.log.Warn("ota: no ack progress within otaWriteWait", "job", d.jobID, "offset", base,
			"attempt", timeouts-1, "waitedMs", otaWriteWait.Milliseconds())
		if timeouts > otaWriteRetries {
			d.svc.log.Warn("ota: write ack timed out after all retries", "job", d.jobID, "offset", base,
				"attempts", otaWriteRetries+1)
			if d.resume != nil && resumes < otaMaxResumes {
				resumes++
				to, resumed := d.resume(base)
				if !resumed {
					return base, false // resume() has already ended the job
				}
				d.svc.log.Info("ota: resumed after link loss", "job", d.jobID, "from", base, "to", to, "resumes", resumes)
				restart(to)
				continue
			}
			d.done("error", "timeout waiting for write ack", base)
			return base, false
		}
		keep := timeouts
		restart(base)
		timeouts = keep
	}
	return base, true
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

func (d *otaDriver) awaitWriteReply(offset uint16, timeout time.Duration) (nodelib.FirmwareWriteReply, bool) {
	deadline := time.After(timeout)
	for {
		select {
		case r := <-d.writeReplies:
			// A late ack (a resend was answered too, or the link delivered a
			// burst after a stall) belongs to an earlier chunk: taking it for
			// this one would read as a chunk CRC mismatch. A Nack names the
			// offset to resync to, so it is always for the current write.
			if !r.Nack && r.Offset != offset {
				continue
			}
			return r, true
		case <-deadline:
			return nodelib.FirmwareWriteReply{}, false
		}
	}
}
