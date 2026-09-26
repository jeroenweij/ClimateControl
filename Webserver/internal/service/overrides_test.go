package service

import (
	"context"
	"testing"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func TestSettingADamperTargetHoldsManualAndOtherModesDropTheTarget(t *testing.T) {
	svc, _ := newTestService(t)
	ctx := context.Background()
	held := func() map[string]float64 {
		ovs, err := svc.Store().Overrides(ctx, 2)
		if err != nil {
			t.Fatal(err)
		}
		out := map[string]float64{}
		for _, o := range ovs {
			out[o.Endpoint] = o.Value
		}
		return out
	}

	if err := svc.SendCommand(ctx, 2, nodelib.EndpointDamperTarget, 35, "test"); err != nil {
		t.Fatal(err)
	}
	if h := held(); h["DamperTarget"] != 35 || h["DamperMode"] != damperModeManual {
		t.Fatalf("after target: held %v, want DamperTarget 35 + DamperMode Manual", h)
	}

	if err := svc.SendCommand(ctx, 2, nodelib.EndpointDamperMode, 2, "test"); err != nil { // Auto
		t.Fatal(err)
	}
	h := held()
	if _, ok := h["DamperTarget"]; ok || h["DamperMode"] != 2 {
		t.Fatalf("after Auto: held %v, want DamperMode Auto and no DamperTarget", h)
	}
}
