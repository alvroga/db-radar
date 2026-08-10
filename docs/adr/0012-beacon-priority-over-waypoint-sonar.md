# ADR-0012: Beacon takes absolute priority over the fixed-waypoint sonar

Status: Accepted
Date: 2026-07-31
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

Field report: the beacon appeared completely silent. The diagnosed root cause was a pre-existing bug,
unrelated to any prior work that day — `updateWaypointFixSonar()` (`navigation.cpp`) called
`beacon_proximity::suppressSonar(true)` unconditionally the instant a waypoint was fixed at 50m zoom,
*before* checking whether that waypoint was actually within sonar range. A waypoint fixed beyond 50m
therefore produced tempo 0 → `stopSonar()`, permanently muting the beacon with nothing else audible to
explain why (backlog §7.5, report 1). That bug fix is not this decision.

What the bug exposed was a real, previously-unmade choice about how the two sonars should share the
single buzzer when both a fixed waypoint and an in-range beacon are active at once. The narrower fix —
have the waypoint yield the buzzer only when it would actually be beeping — was one option. It was
judged insufficient: a fix left in place still keeps every *other* waypoint hidden from the radar (a
side effect of the fix mechanism, not of its sonar), and would re-claim the buzzer the instant the
beacon's confirmed zone dipped back to OUT_OF_RANGE, fighting the beacon for the buzzer repeatedly
through a single approach.

## Decision

Commit `96b6fa3`. When `beacon_proximity::isInRange()` — scanning, not found, confirmed zone ≠
OUT_OF_RANGE — becomes true, `updateWaypointFixSonar()` **releases the fixed waypoint outright**,
rather than merely yielding the buzzer to the beacon. A beacon is a thing you are trying to *find*; a
fixed waypoint is an area you are walking into, and its sonar is a secondary convenience.

## Consequences

**Easier**: no scenario where a fixed waypoint re-silences or re-interrupts the beacon mid-hunt.
`isInRange()` reads the *confirmed* zone, which already requires 1000ms of hysteresis-gated agreement
to enter (ADR-0011), so the release itself cannot flicker even though it fires every update cycle.

**Harder**: fixing a waypoint no longer guarantees it stays fixed for the rest of a session — a user
relying on both features at once loses the waypoint fix, and the "every other waypoint hidden while
fixed" behavior that comes with it, silently the moment beacon range is entered. That interaction is
now something a user has to learn rather than something the UI states.

**Gave up**: the alternative (buzzer-yield only, waypoint stays fixed) was considered and rejected as
insufficient, not as a cost traded for something else — it doesn't fully solve the problem it was
meant to solve, so nothing of real value was given up by not choosing it.
