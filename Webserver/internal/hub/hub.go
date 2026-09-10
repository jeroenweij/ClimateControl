// Package hub holds the in-memory current-state cache (last value per
// node/endpoint) and fans events out to WebSocket subscribers. See
// MainController-Server-Link-Spec.md §6.
package hub

import (
	"encoding/json"
	"sync"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// ValueEvent is one decoded reading, as cached and pushed to browsers.
type ValueEvent struct {
	Type     string        `json:"type"` // always "value"
	Node     int           `json:"node"`
	Endpoint string        `json:"endpoint"`
	Value    nodelib.Value `json:"value"`
	TS       int64         `json:"ts"` // unix millis
}

// PresenceEvent marks a node joining or leaving the bus.
type PresenceEvent struct {
	Type   string `json:"type"` // "presence"
	Node   int    `json:"node"`
	Module string `json:"module"`
	Up     bool   `json:"up"`
}

// MainEvent carries the periodic MainController/bus health.
type MainEvent struct {
	Type   string             `json:"type"` // "main"
	Status nodelib.MainStatus `json:"status"`
	Online bool               `json:"online"`
	TS     int64              `json:"ts"`
}

// OtaEvent carries firmware-push progress.
type OtaEvent struct {
	Type   string `json:"type"` // "ota"
	JobID  int64  `json:"jobId"`
	State  string `json:"state"`
	NodeID int    `json:"nodeId"`
	Offset int    `json:"offset"`
	Size   int    `json:"size"`
	Error  string `json:"error,omitempty"`
}

type stateKey struct {
	node int
	ep   nodelib.Endpoint
}

// Hub is safe for concurrent use.
type Hub struct {
	mu       sync.RWMutex
	values   map[stateKey]ValueEvent
	main     MainEvent
	subs     map[chan []byte]struct{}
	uplinkUp bool
}

// New returns an empty hub.
func New() *Hub {
	return &Hub{
		values: make(map[stateKey]ValueEvent),
		subs:   make(map[chan []byte]struct{}),
	}
}

// Subscribe registers a subscriber channel and returns it plus an unsubscribe
// func. The channel is buffered; a subscriber that cannot keep up is dropped.
func (h *Hub) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, 64)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	h.mu.Unlock()
	return ch, func() {
		h.mu.Lock()
		if _, ok := h.subs[ch]; ok {
			delete(h.subs, ch)
			close(ch)
		}
		h.mu.Unlock()
	}
}

func (h *Hub) broadcast(v any) {
	msg, err := json.Marshal(v)
	if err != nil {
		return
	}
	h.mu.RLock()
	defer h.mu.RUnlock()
	for ch := range h.subs {
		select {
		case ch <- msg:
		default: // slow subscriber: drop this message for it
		}
	}
}

// PublishValue caches and broadcasts a decoded reading.
func (h *Hub) PublishValue(node int, ep nodelib.Endpoint, v nodelib.Value) {
	ev := ValueEvent{
		Type:     "value",
		Node:     node,
		Endpoint: ep.String(),
		Value:    v,
		TS:       time.Now().UnixMilli(),
	}
	h.mu.Lock()
	h.values[stateKey{node, ep}] = ev
	h.mu.Unlock()
	h.broadcast(ev)
}

// PublishPresence broadcasts a node up/down transition.
func (h *Hub) PublishPresence(node int, module nodelib.Module, up bool) {
	h.broadcast(PresenceEvent{Type: "presence", Node: node, Module: module.String(), Up: up})
}

// PublishThermostat broadcasts a paired-thermostat state change so the
// Firmware view can refresh.
func (h *Hub) PublishThermostat(controllerNodeID int, linkUp bool) {
	h.broadcast(map[string]any{
		"type": "thermostat", "controllerNodeId": controllerNodeID, "linkUp": linkUp,
	})
}

// PublishMain caches and broadcasts MainController health.
func (h *Hub) PublishMain(st nodelib.MainStatus, online bool) {
	ev := MainEvent{Type: "main", Status: st, Online: online, TS: time.Now().UnixMilli()}
	h.mu.Lock()
	h.main = ev
	h.uplinkUp = online
	h.mu.Unlock()
	h.broadcast(ev)
}

// SetUplink records and broadcasts just the socket up/down state.
func (h *Hub) SetUplink(up bool) {
	h.mu.Lock()
	changed := h.uplinkUp != up
	h.uplinkUp = up
	m := h.main
	h.mu.Unlock()
	if changed {
		m.Type = "main"
		m.Online = up
		m.TS = time.Now().UnixMilli()
		h.broadcast(m)
	}
}

// PublishOta broadcasts firmware-push progress.
func (h *Hub) PublishOta(ev OtaEvent) {
	ev.Type = "ota"
	h.broadcast(ev)
}

// Snapshot is the full current state sent to a browser on connect.
type Snapshot struct {
	Type     string       `json:"type"` // "snapshot"
	Values   []ValueEvent `json:"values"`
	Main     MainEvent    `json:"main"`
	UplinkUp bool         `json:"uplinkUp"`
}

// SnapshotJSON returns the marshalled current state.
func (h *Hub) SnapshotJSON() []byte {
	h.mu.RLock()
	snap := Snapshot{
		Type:     "snapshot",
		Values:   make([]ValueEvent, 0, len(h.values)),
		Main:     h.main,
		UplinkUp: h.uplinkUp,
	}
	for _, v := range h.values {
		snap.Values = append(snap.Values, v)
	}
	h.mu.RUnlock()
	msg, _ := json.Marshal(snap)
	return msg
}

// CurrentValue returns the last cached value for a node/endpoint.
func (h *Hub) CurrentValue(node int, ep nodelib.Endpoint) (ValueEvent, bool) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	v, ok := h.values[stateKey{node, ep}]
	return v, ok
}
