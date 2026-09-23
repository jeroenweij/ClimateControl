package store

import (
	"context"
	"fmt"
	"testing"
)

func TestPruneOtaJobs(t *testing.T) {
	st := testStore(t)
	ctx := context.Background()

	// Job 1 is still queued (never finishes); jobs 2..(OtaJobsKept+10) are done.
	// The queued one is older than the retention cutoff and must survive.
	mk := func(i int, state string) int64 {
		id, err := st.CreateOtaJob(ctx, OtaJob{NodeID: 1, Filename: "x.bin", ImagePath: fmt.Sprintf("/img/%d", i)})
		if err != nil {
			t.Fatal(err)
		}
		if state != "queued" {
			if err := st.UpdateOtaJob(ctx, id, state, 0, ""); err != nil {
				t.Fatal(err)
			}
		}
		return id
	}
	queuedID := mk(1, "queued")
	total := OtaJobsKept + 10
	for i := 2; i <= total; i++ {
		mk(i, "done")
	}

	paths, err := st.PruneOtaJobs(ctx)
	if err != nil {
		t.Fatal(err)
	}

	jobs, err := st.OtaJobs(ctx, 1000)
	if err != nil {
		t.Fatal(err)
	}
	// Newest OtaJobsKept records stay; the old queued job also stays.
	if len(jobs) != OtaJobsKept+1 {
		t.Fatalf("kept %d jobs, want %d (newest %d + the old queued one)", len(jobs), OtaJobsKept+1, OtaJobsKept)
	}
	if _, err := st.OtaJob(ctx, queuedID); err != nil {
		t.Errorf("old queued job was pruned: %v", err)
	}
	if jobs[0].ID != int64(total) {
		t.Errorf("newest job missing, top id = %d", jobs[0].ID)
	}
	// 110 records, 100 kept by recency => the 10 oldest are candidates, minus the queued one.
	if len(paths) != 9 {
		t.Errorf("returned %d image paths to delete, want 9", len(paths))
	}

	// Under the cap: nothing happens.
	if paths, err := st.PruneOtaJobs(ctx); err != nil || len(paths) != 0 {
		t.Errorf("second prune: paths=%v err=%v, want none", paths, err)
	}
}
