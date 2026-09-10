package service

import "errors"

// ErrNotWritable is returned when a command targets an endpoint the server has
// no encoder for.
var ErrNotWritable = errors.New("endpoint is not writable")

// ErrDownlinkUnavailable is returned when no MainController is connected or its
// outbound queue is full.
var ErrDownlinkUnavailable = errors.New("downlink unavailable (no MainController or queue full)")

// ErrOtaBusy is returned when a firmware push is already running.
var ErrOtaBusy = errors.New("a firmware push is already in progress")

// ErrOtaTargetMismatch is returned when the uploaded image's module does not
// match the chosen push target (a Thermostat image for a "node" push, or a
// non-Thermostat image for a "thermostat" push).
var ErrOtaTargetMismatch = errors.New("image module does not match the selected target")
