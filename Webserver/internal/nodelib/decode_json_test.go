package nodelib

import (
	"encoding/json"
	"strings"
	"testing"
)

// A zero reading must still carry "num" -- the UI formats it directly, and a
// missing field (omitempty) broke every value that happened to be 0.
func TestZeroValueKeepsNumInJSON(t *testing.T) {
	b, err := json.Marshal(DecodeValue(EndpointDamperTarget, []byte{0}))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(b), `"num":0`) {
		t.Fatalf("zero DamperTarget marshals as %s, want a \"num\":0 field", b)
	}
}
