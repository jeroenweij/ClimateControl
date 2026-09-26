package nodelib

import (
	"reflect"
	"testing"
)

func TestFaultNames(t *testing.T) {
	cases := []struct {
		m     Module
		flags uint16
		want  []string
	}{
		{ModuleControllerNode, 0, nil},
		{ModuleControllerNode, 0x0002, []string{"Damper stalled"}},
		{ModuleControllerNode, 0x0003, []string{"Thermostat link down", "Damper stalled"}},
		{ModuleTemperatureNode, 0x0002, []string{"Supply sensor fault"}},
		{ModuleControllerNode, 0x0100, []string{"fault bit 8"}}, // unknown bit still shown
		{ModuleUnknown, 0x0001, []string{"fault bit 0"}},
	}
	for _, c := range cases {
		if got := FaultNames(c.m, c.flags); !reflect.DeepEqual(got, c.want) {
			t.Errorf("FaultNames(%v, %#04x) = %v, want %v", c.m, c.flags, got, c.want)
		}
	}
}
