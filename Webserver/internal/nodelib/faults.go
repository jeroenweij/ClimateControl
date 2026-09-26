package nodelib

import "fmt"

// faultBits names each module's SystemStatus errorFlags bits, index = bit
// number. The meanings are the firmware's own per-module ErrorBit enums
// (ControllerHandler.h, TemperatureHandler.h) -- keep the two in sync.
var faultBits = map[Module][]string{
	ModuleControllerNode:  {"Thermostat link down", "Damper stalled"},
	ModuleTemperatureNode: {"Return sensor fault", "Supply sensor fault"},
}

// FaultNames turns a node's errorFlags into readable names, lowest bit
// first. A bit the table doesn't know (newer firmware, unknown module) still
// shows up, as "fault bit N", rather than being dropped.
func FaultNames(m Module, flags uint16) []string {
	var out []string
	names := faultBits[m]
	for bit := 0; bit < 16; bit++ {
		if flags&(1<<bit) == 0 {
			continue
		}
		if bit < len(names) {
			out = append(out, names[bit])
		} else {
			out = append(out, fmt.Sprintf("fault bit %d", bit))
		}
	}
	return out
}
