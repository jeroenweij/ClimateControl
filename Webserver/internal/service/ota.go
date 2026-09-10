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

// Bootloader states reported in OtaControlReport.State, mirroring
// Modules/Bootloader/FirmwareSlave.h.
const (
	blIdle      = 1
	blErasing   = 2
	blReceiving = 3
	blValid     = 4
	blError     = 5
)

const (
	otaChunk      = 27               // bytes per OtaData frame (spec §5)
	otaWindow     = 16               // frames sent before waiting for a report
	otaReportWait = 3 * time.Second  // per-report timeout
	otaStepWait   = 20 * time.Second // per-phase timeout
)

// StartOTA validates the uploaded image, records a job, and launches the push
// driver. Only one push runs at a time.
//
// target is "node" (flash the bus node itself) or "thermostat" (flash the
// Thermostat paired to the ControllerNode at nodeID, relayed over its private
// link — ControllerNode-Thermostat-Link-Spec.md §5). For a thermostat push the
// image's descriptor module must be Thermostat; the MainController routes on
// the module byte in the 0x65 OtaControl frame (spec §5.7).
func (s *Service) StartOTA(ctx context.Context, nodeID int, target, filename string, bin []byte, imageDir string) (int64, error) {
	if target == "" {
		target = "node"
	}
	if target != "node" && target != "thermostat" {
		return 0, ErrOtaTargetMismatch
	}

	s.mu.Lock()
	if s.ota != nil && !s.ota.finished() {
		s.mu.Unlock()
		return 0, ErrOtaBusy
	}
	s.mu.Unlock()

	if !s.send.Connected() {
		return 0, ErrDownlinkUnavailable
	}

	desc, crc, err := nodelib.ParseImage(bin)
	if err != nil {
		return 0, err
	}

	isThermImage := desc.Module == nodelib.ModuleThermostat
	if (target == "thermostat") != isThermImage {
		return 0, ErrOtaTargetMismatch
	}
	// The module byte carried in 0x65 OtaControl is what tells the
	// MainController to drive ThermostatFirmware instead of Firmware; a
	// Thermostat image already carries module=Thermostat, but be explicit.
	pushModule := desc.Module
	if target == "thermostat" {
		pushModule = nodelib.ModuleThermostat
	}

	if err := os.MkdirAll(imageDir, 0o755); err != nil {
		return 0, err
	}
	path := imageDir + "/ota-" + time.Now().Format("20060102-150405") + "-" + filename
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

	d := &otaDriver{
		svc:     s,
		jobID:   jobID,
		nodeID:  nodeID,
		image:   bin,
		crc32:   crc,
		fw:      uint16(int(desc.FWVersionMajor)<<8 | int(desc.FWVersionMinor)),
		module:  pushModule,
		reports: make(chan nodelib.OtaControlReport, 8),
		doneCh:  make(chan struct{}),
	}
	s.mu.Lock()
	s.ota = d
	s.mu.Unlock()
	go d.run()
	return jobID, nil
}

type otaDriver struct {
	svc    *Service
	jobID  int64
	nodeID int
	image  []byte
	crc32  uint32
	fw     uint16
	module nodelib.Module

	reports chan nodelib.OtaControlReport
	doneCh  chan struct{}
	once    sync.Once
}

func (d *otaDriver) onReport(r nodelib.OtaControlReport) {
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

	// 1. Announce the job. The MainController handles EnterBootloader / Begin
	//    on the bus and reports back via 0x65 (spec §8 steps 2-4).
	set := nodelib.OtaControlSet{
		TargetNodeID: uint8(d.nodeID),
		ImageSize:    uint32(len(d.image)),
		ImageCRC32:   d.crc32,
		FWVersion:    d.fw,
		Module:       d.module,
	}
	d.svc.send.SendSet(d.nodeID, nodelib.EndpointOtaControl, set.Encode())

	if !d.waitFor(blReceiving, otaStepWait) {
		d.done("error", "node did not enter bootloader / begin", 0)
		return
	}
	d.progress("writing", 0)

	// 2. Stream OtaData; the report's NextOffset is authoritative.
	offset := 0
	for offset < len(d.image) {
		for i := 0; i < otaWindow && offset < len(d.image); i++ {
			end := offset + otaChunk
			if end > len(d.image) {
				end = len(d.image)
			}
			d.svc.send.Send(nodelib.Frame{
				Node:      uint8(d.nodeID),
				Endpoint:  nodelib.EndpointOtaData,
				Operation: nodelib.OpSet,
				Data:      nodelib.EncodeOtaData(uint32(offset), d.image[offset:end]),
			})
			offset = end
		}
		r, ok := d.awaitReport(otaReportWait)
		if !ok {
			d.done("error", "timeout waiting for write progress", offset)
			return
		}
		if r.State == blError {
			d.done("error", "node reported error "+itoa(int(r.LastError)), int(r.NextOffset))
			return
		}
		// Rewind to whatever the node actually has.
		offset = int(r.NextOffset)
		d.progress("writing", offset)
	}

	// 3. End marker + verify.
	d.svc.send.SendSet(d.nodeID, nodelib.EndpointOtaControl, set.Encode())
	d.progress("verifying", offset)
	if !d.waitFor(blValid, otaStepWait) {
		d.done("error", "image did not verify on the node", offset)
		return
	}
	d.done("done", "", len(d.image))
}

// waitFor drains reports until one reaches state (or blError) or the timeout.
func (d *otaDriver) waitFor(state uint8, timeout time.Duration) bool {
	deadline := time.After(timeout)
	for {
		select {
		case r := <-d.reports:
			if r.State == state {
				return true
			}
			if r.State == blError {
				return false
			}
		case <-deadline:
			return false
		}
	}
}

func (d *otaDriver) awaitReport(timeout time.Duration) (nodelib.OtaControlReport, bool) {
	select {
	case r := <-d.reports:
		return r, true
	case <-time.After(timeout):
		return nodelib.OtaControlReport{}, false
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
