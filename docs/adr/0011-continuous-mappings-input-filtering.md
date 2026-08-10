# ADR-0011: Continuous mappings with input-side filtering, instead of discrete zones with output-side hysteresis

Status: Accepted
Date: 2026-07-31
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

Two independently built sonar tempos — the waypoint-distance sonar and the beacon-RSSI sonar — both
shipped as a discrete-zone ladder with hysteresis/confirmation guarding the *zone*, and both drew the
same shape of field complaint within hours of each other on 2026-07-31. Waypoint sonar (5/10/30/50m
zones, ±3m hysteresis, 1000ms confirmation): "feels very chaotic, a lot of beeping even at a further
distance, is not as progressive as the beacon" — the 10–50m band, where most of an approach happens,
was only two tempi. Beacon sonar (1500/750/500/250ms zones keyed on `state.zone`): "the rate at which
the beeping changes is very difficult to gauge where to go" — most of a search happens inside one
zone, where moving produces no audible change at all (backlog §7.5, §8.1e).

## Decision

For a continuous physical quantity, map it continuously and put the noise filter on the **input** (a
τ-based EMA on distance/RSSI), not hysteresis on the **output** (zone/tempo). Applied twice the same
day:

- **Waypoint sonar** (commit `9952721`): geometric mapping, 2000ms at 50m → 250ms at 2m
  (`interval = 250 · 8^(ln(d/2)/ln 25)`) — equal distance *ratios* give equal tempo *ratios*. The zone
  enum, ±3m hysteresis, and 1000ms confirmation hold are deleted outright, replaced by a τ=1.5s EMA on
  distance computed from measured `dt` (not a sample count, since render rate isn't guaranteed).
- **Beacon sonar** (commit `96b6fa3`): linear-in-dBm mapping, 1500ms at −90dBm → 150ms at −50dBm
  (`interval = 1500 · 0.1^((rssi+90)/40)`) — exact, not approximate, because RSSI ≈ C − 20·log₁₀(d),
  so equal dBm steps are equal distance ratios by the same logic as the waypoint curve.

Hysteresis survives only where a decision is genuinely discrete: whether to beep at all (beacon:
confirmed zone ≠ OUT_OF_RANGE; waypoint: engage ≤50m / release >55m).

Two same-day refinements (commit `b6565ac`) are evidence *for* this principle, not exceptions to it.
Beacon tempo must read `rssi_display` (the slow τ=2.0s EMA), not `rssi_ema` (τ=0.5s) — a first cut used
the fast EMA and the beat was audibly unsteady, because ±3–5dB of standing-still RSSI noise is a ~25%
swing in period over this mapping's 40dB span; a continuous output only glides if the value driving it
is itself smooth. And beep *duration* now encodes trend continuously, interpolated from the raw
regression slope (`trend_slope_dbm_s`, saturating at ±2dBm/s) rather than switched off the 3-state
`MovementTrend` enum — a first cut used the enum and it flapped APPROACHING/STABLE/DEPARTING at random
when standing still, since the slope hovers near zero.

## Consequences

**Easier**: both sonars now respond audibly to the user's own movement at any distance, not only near
a boundary crossing — the original complaint from both field reports. The two systems also converge on
one design vocabulary (τ-EMA on input, continuous output) instead of each accumulating its own
hysteresis constants independently.

**Harder**: choosing the right τ per consumer is now the entire tuning problem, and it is not
one-size-fits-all — `rssi_ema` (0.5s) feeds zone/trend classification, which has its own downstream
hysteresis and wants low latency; `rssi_display` (2.0s) feeds things heard/shown raw, where rhythm
error is judged more harshly than visual lag. Applying the same τ everywhere reintroduces the choppy
beat, as it did once already in this project's own history.

**Gave up**: the musical-BPM framing of the old beacon zones (40/80/120/240 BPM, "andante" through
"prestissimo") — meaningless once tempo is continuous. Nothing of technical substance was traded away;
the discrete versions were the defect being removed, not a competing design with real merits.
