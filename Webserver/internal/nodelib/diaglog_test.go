package nodelib

import "testing"

func TestParseDiagLog(t *testing.T) {
	at, text, ok := ParseDiagLog(append([]byte{0x10, 0x27, 0x01}, "I: hi"...))
	if !ok || at != 0x012710 || text != "I: hi" {
		t.Fatalf("got %d %q %v", at, text, ok)
	}
	at, text, ok = ParseDiagLog([]byte{0x2A, 0, 0}) // drained: uptime only
	if !ok || at != 42 || text != "" {
		t.Fatalf("drained: got %d %q %v", at, text, ok)
	}
	if _, _, ok := ParseDiagLog(nil); ok { // a node without a log ring
		t.Fatal("empty payload accepted")
	}
	if _, _, ok := ParseDiagLog([]byte{1, 2}); ok {
		t.Fatal("short payload accepted")
	}
}
