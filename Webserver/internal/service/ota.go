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
	otaChunk = 27 // bytes per Write frame (spec §5)
	// Found on the bench: the bootloader's UART is plain polling, no
	// interrupt-driven buffer like the app's Hal::Uart -- bursting many
	// Write frames back-to-back (the old otaWindow=16) can outrun it and
	// overwrite the single-deep receive register before Loop() reads it,
	// desyncing the stream badly enough that it stops answering anything,
	// including a plain Discover, until the desync clears. One at a time,
	// paced by waiting for that write's own status report (which itself
	// only arrives on the next round-robin Poll) before sending the next.
	otaWindow     = 1
	otaReportWait = 3 * time.Second  // per-report timeout
	otaStepWait   = 20 * time.Second // per-phase timeout
	otaBootPoll   = 1 * time.Second  // Firmware Get cadence while waiting for the bootloader

	// Found on the bench, even past the ORE/pacing fixes above: an
	// occasional single write's status report still goes missing outright --
	// the same residual risk NodeMaster.pollTimeout's own comment
	// acknowledges for a normal Poll reply on a real bus. Resending the same
	// chunk is safe either way: FirmwareSlave.HandleWrite ignores an offset
	// that doesn't match its own expectedOffset, so a resend after a report
	// (not the write) was the one actually lost is just silently dropped as
	// stale.
	otaWriteRetries = 3
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
			svc:     s,
			jobID:   job.ID,
			nodeID:  job.NodeID,
			target:  job.Target,
			image:   bin,
			crc32:   job.CRC32,
			fw:      uint16(job.FWVersion),
			module:  nodelib.Module(job.Module),
			reports: make(chan nodelib.FirmwareStatusReport, 8),
			doneCh:  make(chan struct{}),
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

	reports chan nodelib.FirmwareStatusReport
	doneCh  chan struct{}
	once    sync.Once
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

func (d *otaDriver) sendSet(data []byte) {
	d.svc.send.Send(nodelib.Frame{Node: uint8(d.nodeID), Endpoint: d.endpoint(), Operation: nodelib.OpSet, Data: data})
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
	d.progress("entering", 0)

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

	r, ok := d.waitFor(otaStepWait, nodelib.BlReceiving)
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
		d.done("error", "node reported error "+itoa(int(r.LastError)), 0)
		return
	}
	d.progress("writing", 0)

	// Stream Write chunks; the report's ExpectedOffset is authoritative.
	offset := 0
	for offset < len(d.image) {
		windowStart := offset
		var r nodelib.FirmwareStatusReport
		var ok bool
		for attempt := 0; attempt <= otaWriteRetries; attempt++ {
			if attempt == 0 {
				o := windowStart
				for i := 0; i < otaWindow && o < len(d.image); i++ {
					end := o + otaChunk
					if end > len(d.image) {
						end = len(d.image)
					}
					d.sendSet(nodelib.EncodeFirmwareWrite(uint32(o), d.image[o:end]))
					o = end
				}
			} else {
				// A retry can't just resend the same chunk: if the write
				// itself landed and only the report describing it was lost,
				// FirmwareSlave.HandleWrite silently ignores a resend at an
				// offset that's no longer its expectedOffset -- no new
				// statusPending, no new report, so blindly repeating the
				// write would just recreate the same timeout forever. A Get
				// probe always gets a fresh report either way (current
				// expectedOffset whether or not the write applied), and the
				// rewind below naturally retries the write next iteration if
				// it turns out it didn't.
				d.svc.send.SendGet(d.nodeID, d.endpoint())
			}
			r, ok = d.awaitReport(otaReportWait)
			if ok {
				break
			}
		}
		if !ok {
			d.done("error", "timeout waiting for write progress", offset)
			return
		}
		if r.State == nodelib.BlError {
			d.done("error", "node reported error "+itoa(int(r.LastError)), int(r.ExpectedOffset))
			return
		}
		// Rewind to whatever the node actually has.
		offset = int(r.ExpectedOffset)
		d.progress("writing", offset)
	}

	// End marker + verify.
	d.sendSet(nodelib.EncodeFirmwareEnd())
	d.progress("verifying", offset)
	r, ok = d.waitFor(otaStepWait, nodelib.BlValid)
	if !ok {
		d.done("error", "image did not verify on the node", offset)
		return
	}
	if r.State == nodelib.BlError {
		d.done("error", "node reported error "+itoa(int(r.LastError)), offset)
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

// waitFor drains reports until one reaches state, reports an error, or the
// (Thermostat-only) already-current guard fires -- or the timeout elapses.
func (d *otaDriver) waitFor(timeout time.Duration, state uint8) (nodelib.FirmwareStatusReport, bool) {
	deadline := time.After(timeout)
	for {
		select {
		case r := <-d.reports:
			if r.State == state || r.State == nodelib.BlError || r.LastError == nodelib.FwErrAlreadyCurrent {
				return r, true
			}
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

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [12]byte
	i := len(b)
	neg := n < 0
	if neg {
		n = -n
	}
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
