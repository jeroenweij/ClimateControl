package store

import (
	"context"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// ExpectedNode is one row of the expected-node roster.
type ExpectedNode struct {
	ID      int    `json:"id"`
	Module  string `json:"module"`
	Name    string `json:"name"`
	Note    string `json:"note"`
	Source  string `json:"source"` // "config" | "ui"
	AddedTS int64  `json:"addedTs"`
}

// RosterNode is the merged view of one node: what the config expects plus what
// has actually been seen on the bus, reduced to a single status.
type RosterNode struct {
	ID        int    `json:"id"`
	Module    string `json:"module"`
	Name      string `json:"name"`
	Note      string `json:"note"`
	Expected  bool   `json:"expected"`
	Seen      bool   `json:"seen"` // ever reported to this server
	Online    bool   `json:"online"`
	FirstSeen int64  `json:"firstSeen"`
	LastSeen  int64  `json:"lastSeen"`
	// Status is one of "online", "offline", "unexpected". "unexpected" wins
	// when a node is on the bus but not in the expected roster; "offline"
	// covers an expected node that is not currently reporting, including the
	// case where no MainController is connected at all.
	Status string `json:"status"`
}

// --- expected-node roster ------------------------------------------------

// ExpectedNodes returns the full expected roster, ascending by id.
func (s *Store) ExpectedNodes(ctx context.Context) ([]ExpectedNode, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, module, name, note, source, added_ts FROM expected_nodes ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ExpectedNode
	for rows.Next() {
		var e ExpectedNode
		var mod int
		if err := rows.Scan(&e.ID, &mod, &e.Name, &e.Note, &e.Source, &e.AddedTS); err != nil {
			return nil, err
		}
		e.Module = nodelib.Module(mod).String()
		out = append(out, e)
	}
	return out, rows.Err()
}

// SetExpectedNode upserts one expected-node entry with the given source
// ("config" or "ui").
func (s *Store) SetExpectedNode(ctx context.Context, id int, module nodelib.Module, name, note, source string) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO expected_nodes (id, module, name, note, source, added_ts)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			module = excluded.module,
			name   = excluded.name,
			note   = excluded.note,
			source = excluded.source`,
		id, int(module), name, note, source, time.Now().Unix())
	return err
}

// DeleteExpectedNode drops one expected-node entry.
func (s *Store) DeleteExpectedNode(ctx context.Context, id int) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM expected_nodes WHERE id = ?`, id)
	return err
}

// SyncConfigExpectedNodes makes the source='config' rows exactly match want:
// entries are upserted and any config row no longer listed is removed. Rows
// added at runtime (source='ui') are left untouched unless their id also
// appears in want, in which case config takes over.
func (s *Store) SyncConfigExpectedNodes(ctx context.Context, want []ExpectedNode) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()

	keep := make(map[int]struct{}, len(want))
	now := time.Now().Unix()
	for _, e := range want {
		keep[e.ID] = struct{}{}
		mod, _ := nodelib.ModuleByName(e.Module)
		if _, err := tx.ExecContext(ctx, `
			INSERT INTO expected_nodes (id, module, name, note, source, added_ts)
			VALUES (?, ?, ?, ?, 'config', ?)
			ON CONFLICT(id) DO UPDATE SET
				module = excluded.module,
				name   = excluded.name,
				note   = excluded.note,
				source = 'config'`,
			e.ID, int(mod), e.Name, e.Note, now); err != nil {
			return err
		}
	}

	rows, err := tx.QueryContext(ctx, `SELECT id FROM expected_nodes WHERE source = 'config'`)
	if err != nil {
		return err
	}
	var stale []int
	for rows.Next() {
		var id int
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		if _, ok := keep[id]; !ok {
			stale = append(stale, id)
		}
	}
	rows.Close()
	for _, id := range stale {
		if _, err := tx.ExecContext(ctx, `DELETE FROM expected_nodes WHERE id = ?`, id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// --- merged roster -----------------------------------------------------

// Roster merges the expected roster with the seen-node table and classifies
// each node. masterOnline is whether a MainController is currently connected;
// when it is false every expected node is reported "offline".
func (s *Store) Roster(ctx context.Context, masterOnline bool) ([]RosterNode, error) {
	seen, err := s.Nodes(ctx)
	if err != nil {
		return nil, err
	}
	expected, err := s.ExpectedNodes(ctx)
	if err != nil {
		return nil, err
	}

	byID := make(map[int]*RosterNode)
	order := make([]int, 0, len(seen)+len(expected))
	get := func(id int) *RosterNode {
		if n, ok := byID[id]; ok {
			return n
		}
		n := &RosterNode{ID: id}
		byID[id] = n
		order = append(order, id)
		return n
	}

	for _, e := range expected {
		n := get(e.ID)
		n.Expected = true
		n.Name = e.Name
		n.Note = e.Note
		if e.Module != "" && e.Module != nodelib.ModuleUnknown.String() {
			n.Module = e.Module
		}
	}
	for _, sn := range seen {
		n := get(sn.ID)
		n.Seen = true
		n.Online = sn.Online && masterOnline
		n.FirstSeen = sn.FirstSeen
		n.LastSeen = sn.LastSeen
		if sn.Module != "" && sn.Module != nodelib.ModuleUnknown.String() {
			n.Module = sn.Module
		}
	}

	sortInts(order)
	out := make([]RosterNode, 0, len(order))
	for _, id := range order {
		n := byID[id]
		if n.Module == "" {
			n.Module = nodelib.ModuleUnknown.String()
		}
		switch {
		case !n.Expected:
			n.Status = "unexpected"
		case n.Online:
			n.Status = "online"
		default:
			n.Status = "offline"
		}
		out = append(out, *n)
	}
	return out, nil
}

func sortInts(a []int) {
	for i := 1; i < len(a); i++ {
		for j := i; j > 0 && a[j-1] > a[j]; j-- {
			a[j-1], a[j] = a[j], a[j-1]
		}
	}
}
