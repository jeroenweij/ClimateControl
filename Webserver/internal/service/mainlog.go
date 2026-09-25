package service

import "time"

// mainLogKeep is how many MainController log lines the server holds -- in
// memory only, not SQLite (MainController-Server-Link-Spec.md §5.1).
const mainLogKeep = 500

// MainLogLine is one line of MainController's log.
type MainLogLine struct {
	// TS is when the MainController logged the line (unix millis). The frame
	// carries its uptime, not wall-clock time, so the line is placed against
	// the UplinkHello that opened the connection.
	TS   int64  `json:"ts"`
	Text string `json:"text"`
}

// OnMainLog records one MainLog line (pushed by the MainController, never
// requested) and streams it to the browsers. Lines logged before the link
// came up arrive right after the hello, with an uptime before the hello's.
func (s *Service) OnMainLog(uptimeSec uint32, text string) {
	s.mu.Lock()
	// 24-bit uptime on the line; signed difference to the hello's, so a line
	// from before the hello lands in the past.
	d := int64((uptimeSec - s.helloUptime) & uptimeMask)
	if d >= 1<<23 {
		d -= 1 << 24
	}
	line := MainLogLine{TS: s.helloWall.Add(time.Duration(d) * time.Second).UnixMilli(), Text: text}
	s.mainLog = append(s.mainLog, line)
	if len(s.mainLog) > mainLogKeep {
		s.mainLog = append([]MainLogLine(nil), s.mainLog[len(s.mainLog)-mainLogKeep:]...)
	}
	s.mu.Unlock()
	s.hb.PublishMainLog(line.TS, line.Text)
}

// MainLog returns a copy of the held MainController log, oldest first.
func (s *Service) MainLog() []MainLogLine {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]MainLogLine{}, s.mainLog...)
}
