package nodelib

// ParseDiagLog decodes a DiagLog Report (Node-Message-Model-Spec.md §3):
// uptimeSec(3 LE) then the text (≤ 32 characters). A report with no text means
// the node's ring is drained, and its uptime is "now" on the node's clock.
// The uptime is the node's seconds since boot when the line was logged (a
// 24-bit counter that wraps after ~194 days). ok is false for a payload too
// short to carry the uptime -- which is also what a node without a log ring
// answers (an empty Report).
func ParseDiagLog(data []byte) (uptimeSec uint32, text string, ok bool) {
	if len(data) < 3 {
		return 0, "", false
	}
	return uint32(data[0]) | uint32(data[1])<<8 | uint32(data[2])<<16, string(data[3:]), true
}
