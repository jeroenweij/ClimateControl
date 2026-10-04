package hub

import (
	"encoding/json"
	"testing"
	"time"

	"climatecontrol/webserver/internal/nodelib"
)

func TestAMeasurementThatStopsArrivingIsExpired(t *testing.T) {
	h := New()
	ch, unsub := h.Subscribe()
	defer unsub()

	h.PublishValue(3, nodelib.EndpointRoomTemp, nodelib.Value{Kind: "number", Num: 21})
	h.PublishValue(3, nodelib.EndpointRoomSetpoint, nodelib.Value{Kind: "number", Num: 21.5})
	<-ch
	<-ch

	if n := h.ExpireStale(time.Now(), MeasurementStaleAfter); n != 0 {
		t.Fatalf("expired %d fresh values", n)
	}

	later := time.Now().Add(MeasurementStaleAfter + time.Second)
	if n := h.ExpireStale(later, MeasurementStaleAfter); n != 1 {
		t.Fatalf("expired %d values, want just the room temperature", n)
	}
	if _, ok := h.CurrentValue(3, nodelib.EndpointRoomTemp); ok {
		t.Error("stale RoomTemp still cached")
	}
	if _, ok := h.CurrentValue(3, nodelib.EndpointRoomSetpoint); !ok {
		t.Error("a setpoint is a state, not a measurement -- it must stay")
	}

	var ev ExpiredEvent
	if err := json.Unmarshal(<-ch, &ev); err != nil || ev.Type != "expired" || ev.Node != 3 || ev.Endpoint != "RoomTemp" {
		t.Fatalf("broadcast %+v (err %v), want an expired event for 3/RoomTemp", ev, err)
	}
}
