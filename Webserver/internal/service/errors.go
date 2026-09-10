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
