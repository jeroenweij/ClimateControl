package service

import (
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// How long to wait for MainController to reset into its bootloader and bring
// the uplink back up: reset, NINA boot, Wi-Fi join, TCP connect, UplinkHello.
const otaMainBootWait = 120 * time.Second

// otaMainWindow is how many chunks a MainController push keeps in flight. The
// bootloader takes chunks in order and answers each one, so a small window is
// enough to keep the link continuously busy; a lost chunk costs at most the
// window's worth of resends.
const otaMainWindow = 4

// How long a transfer waits for the uplink to come back after it goes quiet
// mid-write: the MainController notices within ~20 s (an unanswered keepalive),
// resets the NINA and reconnects in ~10 s. A var only so tests can shrink it.
var otaMainResumeWait = 120 * time.Second

// runMainController drives a MainController self-update. MainController is
// the bus master with no relay target for itself, so instead of the bus
// Firmware sequence it is parked in its own bootloader (SystemControl to
// NODE 0), which dials the uplink back out and speaks OtaControl / OtaData
// (MainController-Server-Link-Spec.md §5, §8 step 7):
//
//	SystemControl[reset->bootloader] -> reconnect -> OtaControl[Begin] ->
//	OtaData* -> OtaControl[End] -> OtaControl[Activate]
func (d *otaDriver) runMainController() {
	d.progress("entering bootloader", 0)
	if !d.enterMainBootloader() {
		d.done("error", "MainController did not enter its bootloader", 0)
		return
	}

	d.progress("erasing", 0)
	if !d.mainControl(nodelib.EncodeOtaBegin(uint32(len(d.image)), d.crc32, d.fw), nodelib.BlReceiving, "begin") {
		return
	}

	d.progress("writing", 0)
	d.resume = d.resumeMainController
	d.window = otaMainWindow
	offset, ok := d.writeImage(nodelib.EndpointOtaData, nodelib.EncodeOtaData)
	if !ok {
		return
	}

	// Every chunk was individually verified against what this driver meant
	// to send, so a whole-image CRC32 failure here points at a server-side
	// chunking bug rather than the flash path (see writeImage).
	d.progress("verifying", offset)
	if !d.mainControl(nodelib.EncodeOtaEnd(), nodelib.BlValid, "verify") {
		return
	}

	// No reply: Activate resets straight into the new application.
	d.sendFrame(nodelib.EndpointOtaControl, nodelib.OpSet, nodelib.EncodeOtaActivate())
	d.done("done", "", len(d.image))
}

// resumeMainController is writeImage's answer to a chunk that went unanswered
// through every retry. The NINA can stop forwarding the MainController's bytes
// for tens of seconds; the MainController then resets it and dials back in,
// and its bootloader (which the NINA reset does not touch) still holds
// everything received so far. So: wait for a status Report and carry on from
// the offset it names. A bootloader that has lost its state (the MCU itself
// restarted) is sent Begin again and the image starts over. Returns the offset
// to continue from; when it returns false the job has already been ended.
func (d *otaDriver) resumeMainController(offset int) (int, bool) {
	d.progress("reconnecting", offset)
	d.drainReplies()

	deadline := time.After(otaMainResumeWait)
	ticker := time.NewTicker(otaBootPoll)
	defer ticker.Stop()
	for {
		select {
		case r := <-d.reports:
			switch r.State {
			case nodelib.BlReceiving:
				next := int(r.ExpectedOffset)
				if next > len(d.image) {
					d.done("error", "MainController reports an impossible resume offset", offset)
					return 0, false
				}
				d.drainReplies()
				d.progress("writing", next)
				return next, true
			case nodelib.BlError:
				d.failMain("resume", r.LastError)
				return 0, false
			default:
				// Idle: the bootloader restarted and has nothing. Start over.
				d.progress("erasing", 0)
				if !d.mainControl(nodelib.EncodeOtaBegin(uint32(len(d.image)), d.crc32, d.fw), nodelib.BlReceiving, "begin") {
					return 0, false
				}
				d.progress("writing", 0)
				return 0, true
			}
		case <-ticker.C:
			d.svc.send.SendGet(0, nodelib.EndpointOtaControl) // dropped while the link is down
		case <-deadline:
			d.done("error", "MainController did not come back after the link was lost", offset)
			return 0, false
		}
	}
}

// drainReplies discards anything queued from an earlier phase, so a stale
// status report can't be mistaken for the answer to the next request.
func (d *otaDriver) drainReplies() {
	for {
		select {
		case <-d.reports:
		case <-d.opReplies:
		case <-d.writeReplies:
		default:
			return
		}
	}
}

// enterMainBootloader commands MainController into its bootloader and waits
// for the bootloader's OtaControl status Report. Only the bootloader ever
// answers OtaControl Get (the running app ignores it), so any report proves
// the reset landed -- and if MainController was already resident there (no
// valid app, or a repeat push), the SystemControl is simply dropped and the
// first probe is answered.
func (d *otaDriver) enterMainBootloader() bool {
	d.drainReplies()
	d.sendFrame(nodelib.EndpointSystemControl, nodelib.OpSet, []byte{nodelib.SystemControlResetToBootloader})

	deadline := time.After(otaMainBootWait)
	ticker := time.NewTicker(otaBootPoll)
	defer ticker.Stop()
	for {
		select {
		case <-d.reports:
			return true
		case <-ticker.C:
			d.svc.send.SendGet(0, nodelib.EndpointOtaControl)
		case <-deadline:
			return false
		}
	}
}

// mainControl sends one OtaControl Set and waits until the bootloader has
// reached state want. An Ack settles it immediately; a Nack (or an Error
// status) fails the push. Status probes (OtaControl Get) run alongside, and a
// probe that still shows an earlier state well after the Set means the Set
// itself was lost, so it is sent again -- safe for Begin (re-erases, nothing
// received yet); End is only re-sent while still Receiving, before it has
// taken effect.
func (d *otaDriver) mainControl(payload []byte, want uint8, what string) bool {
	d.drainReplies()

	sentAt := time.Now()
	d.sendFrame(nodelib.EndpointOtaControl, nodelib.OpSet, payload)

	deadline := time.After(otaStepWait)
	ticker := time.NewTicker(otaBootPoll)
	defer ticker.Stop()
	for {
		select {
		case r := <-d.opReplies:
			if r.Nack {
				d.failMain(what, r.LastError)
				return false
			}
			return true
		case r := <-d.reports:
			switch {
			case r.State == want:
				return true
			case r.State == nodelib.BlError:
				d.failMain(what, r.LastError)
				return false
			case time.Since(sentAt) > 2*otaBootPoll:
				sentAt = time.Now()
				d.sendFrame(nodelib.EndpointOtaControl, nodelib.OpSet, payload)
			}
		case <-ticker.C:
			d.svc.send.SendGet(0, nodelib.EndpointOtaControl)
		case <-deadline:
			d.done("error", "MainController did not answer "+what, 0)
			return false
		}
	}
}

func (d *otaDriver) failMain(what string, lastError uint8) {
	if what == "verify" && lastError == nodelib.FwErrCrcMismatch {
		d.done("error", "image verified chunk-by-chunk during transfer but failed the final image CRC check "+
			"-- likely a server-side chunking bug, not a flash issue", 0)
		return
	}
	d.done("error", "MainController "+what+": "+nodelib.FirmwareErrorName(lastError), 0)
}
