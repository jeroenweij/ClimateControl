package nodelib

// DiagLastError codes -- mirror Software/Lib/HAL/Fault.h (Hal::Fault::Code).
const (
	FaultNone      = 0
	FaultHardFault = 1 // context = faulting PC - FlashBase
	FaultWatchdog  = 2 // uptimeAtFault = uptime at the last watchdog feed
)

// FlashBase is where the STM32G031's flash is mapped: a HardFault's context
// is the faulting PC as an offset from it.
const FlashBase = 0x08000000

// FaultCodeName names a DiagLastError code.
func FaultCodeName(code uint8) string {
	switch code {
	case FaultNone:
		return "none"
	case FaultHardFault:
		return "HardFault"
	case FaultWatchdog:
		return "Watchdog"
	}
	return "unknown"
}

// ResetCauseNames lists the reset flags set in SystemStatus.resetCause --
// RCC_CSR[31:24] of the device, latched and cleared at its application's
// startup, so they describe the reset(s) that led to this boot.
func ResetCauseNames(cause uint8) []string {
	names := []string{}
	for _, f := range []struct {
		bit  uint8
		name string
	}{
		{1 << 1, "option-byte load"},
		{1 << 2, "NRST pin"},
		{1 << 3, "power-on/brown-out"},
		{1 << 4, "software"},
		{1 << 5, "independent watchdog"},
		{1 << 6, "window watchdog"},
		{1 << 7, "low-power"},
	} {
		if cause&f.bit != 0 {
			names = append(names, f.name)
		}
	}
	return names
}
