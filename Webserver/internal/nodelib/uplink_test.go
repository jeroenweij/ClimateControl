package nodelib

import "testing"

func TestParseNodePresence(t *testing.T) {
	p, ok := ParseNodePresence([]byte{4, byte(ModuleControllerNode), 1, 1})
	if !ok || p.NodeID != 4 || p.Module != ModuleControllerNode || !p.Up || !p.Bootloader {
		t.Fatalf("up+bootloader: got %+v ok=%v", p, ok)
	}

	p, ok = ParseNodePresence([]byte{4, byte(ModuleControllerNode), 1, 0})
	if !ok || !p.Up || p.Bootloader {
		t.Fatalf("up, app mode: got %+v ok=%v", p, ok)
	}

	p, ok = ParseNodePresence([]byte{4, byte(ModuleControllerNode), 0, 0})
	if !ok || p.Up {
		t.Fatalf("down: got %+v ok=%v", p, ok)
	}

	// Pre-bootloader-bit payloads (3 bytes) no longer parse -- the two sides
	// are byte-compatible by construction (CLAUDE.md), not by tolerating a
	// shorter legacy shape.
	if _, ok := ParseNodePresence([]byte{4, byte(ModuleControllerNode), 1}); ok {
		t.Fatalf("3-byte payload parsed; want false now that Bootloader is required")
	}
}

func TestParseRosterEntry(t *testing.T) {
	e, ok := ParseRosterEntry([]byte{4, byte(ModuleControllerNode), 1, 0x78, 0x56, 0x34, 0x12})
	if !ok || e.NodeID != 4 || e.Module != ModuleControllerNode || e.State != 1 || e.LastSeenMs != 0x12345678 {
		t.Fatalf("got %+v ok=%v", e, ok)
	}

	if _, ok := ParseRosterEntry([]byte{4, byte(ModuleControllerNode), 1}); ok {
		t.Fatalf("short payload parsed; want false")
	}
}
