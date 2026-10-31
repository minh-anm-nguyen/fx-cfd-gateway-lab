# Fixtures

This directory holds no binary fixture files, and that is deliberate.

Design doc §15.2 step 2 asks for "a fixed simulator fixture/seed" and §23
requires every case artifact to carry the seed that reproduces it. Both are
satisfied by generating the fixture from a recorded seed rather than by checking
in a blob:

- the generator is in the test that uses it, so what a fixture contains is
  readable next to the assertions about it;
- the seed is written into the suite artifact (`"seed"` field) and into the run
  manifest (`workload.seed`), so a failure reproduces from the artifact alone;
- the SHA-256 of the generated event stream goes into the manifest as
  `workload.fixture_sha256`, so two runs can prove they replayed the same
  stream — which is what makes the baseline-versus-optimized comparison fair
  (§15.3, "unfair comparison");
- a checked-in blob would drift from the generator that produced it, and
  nothing would notice.

`replay_benchmark --seed=N` and the `make_fixture` helpers in
`tests/differential/` and `tests/model/` are the generators.

## What the path is used for

CMake passes `LLX_FIXTURES_DIR` to every suite. The suites use it as the anchor
for `build/test-artifacts/`, where the crash and differential cases write the
journals they then re-open after a simulated process death. Those files are
created and deleted by the cases that own them, so nothing accumulates here.

## When a real fixture file would be needed

A captured FIX byte stream, which belongs to P01 and P02. Those adapters are not
wired in — `LL-511` records that as `BLOCKED` — so there is nothing to capture
yet. When they are, a recorded stream goes here with its SHA-256 alongside, and
the generator stays for the synthetic cases.
