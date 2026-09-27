package store

import (
	"context"
	"math"
	"testing"
	"time"

	"climatecontrol/webserver/internal/nodelib"
)

const minuteMs = int64(time.Minute / time.Millisecond)

func insertNum(t *testing.T, st *Store, ts int64, ep nodelib.Endpoint, v float64) {
	t.Helper()
	raw := []byte{byte(v), byte(int(v*100) % 100)}
	if err := st.InsertReading(context.Background(), ts, 1, ep, raw, nodelib.Value{Kind: "number", Num: v}); err != nil {
		t.Fatal(err)
	}
}

func count(t *testing.T, st *Store, table string) int {
	t.Helper()
	var n int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM ` + table).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func near(a, b float64) bool { return math.Abs(a-b) < 1e-9 }

func TestInsertReadingSkipsRepeatsWithinRepeatInterval(t *testing.T) {
	st := testStore(t)
	ep := nodelib.EndpointRoomTemp
	t0 := int64(1_000_000_000_000)

	insertNum(t, st, t0, ep, 21.5)
	insertNum(t, st, t0+minuteMs, ep, 21.5)                             // keepalive repeat: dropped
	insertNum(t, st, t0+2*minuteMs, ep, 21.6)                           // change: stored
	insertNum(t, st, t0+3*minuteMs, ep, 21.6)                           // repeat: dropped
	insertNum(t, st, t0+2*minuteMs+RepeatInterval, ep, 21.6)            // repeat, but due again: stored
	insertNum(t, st, t0+3*minuteMs, nodelib.EndpointRoomHumidity, 21.6) // other series

	if got := count(t, st, "readings"); got != 4 {
		t.Fatalf("rows = %d, want 4", got)
	}
}

func TestAggregateHoursIsTimeWeightedAndCapsTheHold(t *testing.T) {
	h0 := int64(100) * hourMs
	carry := &sample{h0 - 30*minuteMs, 10} // holds into the first hour
	samples := []sample{
		{h0 + 15*minuteMs, 20},          // 10 for 15 min, then 20 for 55 min ...
		{h0 + hourMs + 10*minuteMs, 40}, // ... then 40, which holds for holdMs only
	}
	aggs := aggregateHours(carry, samples, h0, h0+4*hourMs)
	if len(aggs) != 3 {
		t.Fatalf("hours = %d, want 3 (the 4th is past the hold)", len(aggs))
	}

	a := aggs[0]
	if a.hour != h0 || a.n != 1 || a.min != 10 || a.max != 20 || !near(a.avg(), (10*15+20*45)/60.0) {
		t.Errorf("hour 0 = %+v avg %v", a, a.avg())
	}
	b := aggs[1]
	if b.n != 1 || b.min != 20 || b.max != 40 || !near(b.avg(), (20*10+40*50)/60.0) {
		t.Errorf("hour 1 = %+v avg %v", b, b.avg())
	}
	c := aggs[2]
	if c.n != 0 || c.dur != 10*minuteMs || !near(c.avg(), 40) {
		t.Errorf("hour 2 = %+v avg %v", c, c.avg())
	}
}

func TestRollUpFoldsOldReadingsIntoHourlyAndDeletesThem(t *testing.T) {
	ctx := context.Background()
	st := testStore(t)
	now := time.UnixMilli(2000 * hourMs)
	keep := 30 * 24 * time.Hour
	cutoff := floorHour(now.Add(-keep).UnixMilli())
	old := cutoff - 3*hourMs

	insertNum(t, st, old, nodelib.EndpointRoomTemp, 20)
	insertNum(t, st, old+30*minuteMs, nodelib.EndpointRoomTemp, 22)
	insertNum(t, st, old+hourMs+10*minuteMs, nodelib.EndpointRoomTemp, 24)
	// A struct value: no hourly row, but deleted all the same.
	if err := st.InsertReading(ctx, old, 1, nodelib.EndpointSystemStatus, []byte{1, 2}, nodelib.Value{Kind: "struct"}); err != nil {
		t.Fatal(err)
	}
	insertNum(t, st, cutoff+hourMs, nodelib.EndpointRoomTemp, 25) // inside the window: kept

	stats, err := st.RollUp(ctx, now, keep)
	if err != nil {
		t.Fatal(err)
	}
	if stats.Deleted != 4 || stats.Series != 2 {
		t.Errorf("stats = %+v, want 4 deleted over 2 series", stats)
	}
	if got := count(t, st, "readings"); got != 1 {
		t.Errorf("raw rows left = %d, want 1", got)
	}

	// 20 for 30 min + 22 for 30 min; 22 for 10 min + 24 for 50 min; 24 for
	// the 10 min of hold left.
	pts, err := st.Readings(ctx, 1, nodelib.EndpointRoomTemp, 0, now.UnixMilli(), 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 4 || pts[0].Min != nil || *pts[0].Num != 25 {
		t.Fatalf("points = %+v, want the raw 25 first, then 3 hourly", pts)
	}
	wantAvg := []float64{24, (22*10 + 24*50) / 60.0, 21}
	for i, p := range pts[1:] {
		if p.TS != old+int64(2-i)*hourMs || !near(*p.Num, wantAvg[i]) {
			t.Errorf("hourly %d = ts %d avg %v, want ts %d avg %v", i, p.TS, *p.Num, old+int64(2-i)*hourMs, wantAvg[i])
		}
	}
	if *pts[2].Min != 22 || *pts[2].Max != 24 || pts[2].N != 1 {
		t.Errorf("hour 1 min/max/n = %v/%v/%d", *pts[2].Min, *pts[2].Max, pts[2].N)
	}

	// A second pass over the same state changes nothing.
	again, err := st.RollUp(ctx, now, keep)
	if err != nil || again.Deleted != 0 || again.Hours != 0 {
		t.Errorf("second pass = %+v, %v", again, err)
	}
}

func TestRollUpCarriesTheHeldValueIntoTheNextPass(t *testing.T) {
	ctx := context.Background()
	st := testStore(t)
	keep := time.Hour
	h := int64(5000) * hourMs

	insertNum(t, st, h+50*minuteMs, nodelib.EndpointRoomTemp, 20)
	insertNum(t, st, h+hourMs+30*minuteMs, nodelib.EndpointRoomTemp, 30)

	// Pass 1 rolls up hour h only; pass 2, an hour later, hour h+1 -- whose
	// first half still holds the 20 from before pass 1's cutoff.
	if _, err := st.RollUp(ctx, time.UnixMilli(h+2*hourMs), keep); err != nil {
		t.Fatal(err)
	}
	if _, err := st.RollUp(ctx, time.UnixMilli(h+3*hourMs), keep); err != nil {
		t.Fatal(err)
	}
	var avg, lo float64
	if err := st.DB().QueryRow(`SELECT avg, min FROM readings_hourly WHERE hour = ?`, h+hourMs).Scan(&avg, &lo); err != nil {
		t.Fatal(err)
	}
	if !near(avg, 25) || lo != 20 {
		t.Errorf("hour h+1 avg/min = %v/%v, want 25/20", avg, lo)
	}
}

func TestRollUpNeverRewritesAFinishedHourFromAStrayRow(t *testing.T) {
	ctx := context.Background()
	st := testStore(t)
	keep := time.Hour
	h := int64(6000) * hourMs

	insertNum(t, st, h+10*minuteMs, nodelib.EndpointRoomTemp, 20)
	if _, err := st.RollUp(ctx, time.UnixMilli(h+2*hourMs), keep); err != nil {
		t.Fatal(err)
	}
	// The server clock stepped back: a row lands in the already rolled hour,
	// long after the carried value ran out.
	insertNum(t, st, h+20*minuteMs, nodelib.EndpointRoomTemp, 99)
	if _, err := st.RollUp(ctx, time.UnixMilli(h+5*hourMs), keep); err != nil {
		t.Fatal(err)
	}
	var avg float64
	if err := st.DB().QueryRow(`SELECT avg FROM readings_hourly WHERE hour = ?`, h).Scan(&avg); err != nil {
		t.Fatal(err)
	}
	if avg != 20 || count(t, st, "readings") != 0 {
		t.Errorf("hour h avg = %v, raw rows = %d; want 20 and 0", avg, count(t, st, "readings"))
	}
}
