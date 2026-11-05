---
name: test-coverage
description: Check, extend and verify test coverage in Siberia. Use when changing the core library, the protocol, the Electron binding or the tools, before a commit, or when asked about tests or coverage.
argument-hint: "[area, optional]"
---

Every behaviour change ships with a test that fails without it.

## What to run

| Area | Run it | Covers |
|---|---|---|
| Core library | `make test` | Real processes: API, crashes with `kill -9`, port leaks, system calls, and tearing |
| Electron binding | `make test-binding` | Formats, scaling, holding frames, closing and reopening, under plain Node |
| Electron end to end | `experiments/electron-test/run.sh` and `runrecv.sh` | Real Electron against the pattern tool |
| Speed | `tools/bench.sh shared` | 4K Metal between two processes |

`make test` must end with the deliberately broken build being caught.

## Where a test goes

- Core behaviour goes in `test/test_siberia.c`, using real child processes for anything about crashes or races.
- Binding behaviour goes in `bindings/electron/test.js`.
- VRS tests the host layer in its own repo. Run its suites too after changing the binding: `python3 test/run.py` there, then `python3 test/all.py` from `app/`.

## Rules

1. **A test must be able to fail.** Prove it for anything important: the broken build exists because 0 torn frames means nothing unless the same test finds tearing when the protection is removed.
2. **Check every pixel for tearing,** and keep the race widened in `test_yield`.
3. **Test crashes for real.** Use `SIGKILL` on either side and check that buffers and places come back.
4. **Keep the per-frame path free of system calls.** `test_no_system_calls` guards this.
5. **Don't assert speed in tests.** Measure it separately, and compare only runs made in the same conditions. Check the GPU load first, because another app can hold it at 100%.
6. **Use unique stream names,** and never kill processes you didn't start.

## Reporting

Say what ran, the counts, what was skipped, and what the numbers were measured under.
