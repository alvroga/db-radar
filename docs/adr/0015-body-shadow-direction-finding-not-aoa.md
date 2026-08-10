# ADR-0015: Body-shadow direction finding instead of BT 5.1 Angle-of-Arrival

Status: Proposed
Date: 2026-07-31
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The beacon proximity feature (`beacon_proximity.cpp`) answers "how far?" via RSSI, but not "which way?"
The natural next question — stand still, rotate, and have the device point toward the beacon — was
evaluated in `docs/beacon_direction_finding.md` (dated 2026-07-31).

**BT 5.1 Angle-of-Arrival/Angle-of-Departure is ruled out by hardware, not by preference.** The
technique requires an antenna array with an RF switch to sample phase difference across elements, plus
access to IQ samples of the advertisement's Constant Tone Extension. This board has one onboard
antenna and no free GPIO for an RF switch (every remaining pin is allocated), and the ESP32-S3 radio
does not produce CTE IQ samples — nor does ESP-IDF's NimBLE expose them if it did. The design doc is
explicit that this is a hard stop: "No amount of software makes a single-antenna radio measure phase
difference. Do not go looking for a library that claims otherwise"
(`docs/beacon_direction_finding.md:16-26`).

The alternative — **body-shadow direction finding** — exploits an established manual RDF technique
instead: the human body attenuates 2.4GHz by roughly 10–20dB, so RSSI dips systematically when the
user's torso is between the phone and the beacon. Extracting that signal needs a heading paired to
every RSSI sample, which the QMC5883L compass already provides at 10Hz (ADR-0017).

This approach was explicitly **blocked on BLE sample rate**, not on the DF math. Binning a 360°
rotation into 12×30° sectors, the pre-fix passive-scan rate of ~2.0Hz yielded only ~1.7 samples per
bin — noise, not signal. ADR-0010 (continuous passive BLE scan) raised the confirmed rate to
4.24–4.37Hz, yielding ~3.6 samples/bin — the design doc's table (`beacon_direction_finding.md:47-51`)
calls this "marginal; workable outdoors." Reconfiguring the tag's advertising interval to 100ms (not
yet done) would reach ~10Hz and ~8.3 samples/bin, the row the doc calls "works."

## Decision

Do not pursue BT 5.1 AoA — it is physically impossible on this hardware. Pursue body-shadow DF
instead, using the first-circular-harmonic bearing estimate (not `argmax`) described in
`docs/beacon_direction_finding.md` §4.2, gated by a confidence threshold (§4.3) that refuses to answer
rather than point somewhere wrong when the RSSI pattern is flat.

As of this writing the feature is **designed and unblocked, but not implemented** — no accumulator
module, no UI, no empirical sign/offset calibration (§5, explicitly flagged as needing measurement, not
derivation, given this project's track record with un-measured RF assumptions). `Status: Proposed`
reflects that; only the AoA rejection above is settled.

## Consequences

**Easier**: the feature has a concrete, hardware-honest algorithm and an explicit operating envelope
(`beacon_direction_finding.md` §6: reliable 10–40m outdoors, unreliable indoors, degenerate under ~5m)
instead of chasing a technique this board cannot perform.

**Harder**: accuracy depends on data the project doesn't have yet — an empirical calibration of the
peak's sign and offset against the device's own asymmetric radiation pattern (PCB ground plane, LCD,
battery), which must be measured at multiple ranges, not assumed.

**Gave up**: true angle-of-arrival precision. Body-shadow DF's stated envelope is "start walking that
way" (±30–45°, a reliable quadrant), not "point at the beacon" — a deliberately lower bar than AoA
would have offered, had AoA been possible here.
