package nodelib

import "testing"

func TestParseFirmwareName(t *testing.T) {
	ok := []struct {
		name    string
		module  Module
		version int
	}{
		{"ControllerNode_1.0.bin", ModuleControllerNode, 1 << 8},
		{"TemperatureNode_2.13.bin", ModuleTemperatureNode, 2<<8 | 13},
		{"Thermostat_0.9.bin", ModuleThermostat, 9},
		{"MainController_10.4.bin", ModuleMainController, 10<<8 | 4},
		{"controllernode_1.0.bin", ModuleControllerNode, 1 << 8},   // case-insensitive
		{"Thermostat-v3.2.bin", ModuleThermostat, 3<<8 | 2},        // dash + v allowed
		{"ControllerNode_1.0.5.bin", ModuleControllerNode, 1 << 8}, // patch ignored
	}
	for _, c := range ok {
		fn, got := ParseFirmwareName(c.name)
		if !got {
			t.Errorf("%s: rejected, want accepted", c.name)
			continue
		}
		if fn.Module != c.module || fn.Version != c.version {
			t.Errorf("%s: got module=%v version=%d, want module=%v version=%d",
				c.name, fn.Module, fn.Version, c.module, c.version)
		}
	}

	bad := []string{
		"", "firmware.bin", "ControllerNode.bin", "ControllerNode_1.bin",
		"ControllerNode_1.0.hex", "Gadget_1.0.bin", "Unknown_1.0.bin",
		"ControllerNode_1.0.bin.bak", "ota-ControllerNode_1.0.bin",
	}
	for _, name := range bad {
		if _, ok := ParseFirmwareName(name); ok {
			t.Errorf("%q: accepted, want rejected", name)
		}
	}
}

func TestFirmwareFileNameRoundTrip(t *testing.T) {
	name := FirmwareFileName(ModuleThermostat, 2<<8|5)
	if name != "Thermostat_2.5.bin" {
		t.Fatalf("got %q", name)
	}
	fn, ok := ParseFirmwareName(name)
	if !ok || fn.Module != ModuleThermostat || fn.Version != 2<<8|5 {
		t.Fatalf("round trip failed: %+v ok=%v", fn, ok)
	}
}
