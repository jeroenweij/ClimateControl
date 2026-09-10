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

// ErrOtaQueued is returned when a job for the same node+target is already
// queued or running, so the repeated request was ignored.
var ErrOtaQueued = errors.New("a firmware push for this target is already queued")

// ErrNoFirmwareImage is returned when no image is held for the module a push
// would need.
var ErrNoFirmwareImage = errors.New("no firmware image uploaded for this module")

// ErrBadFirmwareName is returned when an uploaded file does not match the
// <Module>_<major>.<minor>.bin naming convention.
var ErrBadFirmwareName = errors.New("filename must be <Module>_<major>.<minor>.bin (e.g. ControllerNode_1.0.bin)")

// ErrFirmwareDescMismatch is returned when the image descriptor's module or
// version disagrees with the filename.
var ErrFirmwareDescMismatch = errors.New("image descriptor module/version does not match the filename")

// ErrUnknownNodeModule is returned when a push target's board type cannot be
// determined (needed to pick the right image from the repository).
var ErrUnknownNodeModule = errors.New("cannot determine the node's module type")

// ErrOtaTargetMismatch is returned when the uploaded image's module does not
// match the chosen push target (a Thermostat image for a "node" push, or a
// non-Thermostat image for a "thermostat" push).
var ErrOtaTargetMismatch = errors.New("image module does not match the selected target")
