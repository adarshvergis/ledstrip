---
name: run-tests
description: Run the automated test workflow for the LED strip project - unit tests, test case upkeep, firmware / host software / security tests, and issue reporting. Use when the user asks to test, run the test workflow, or verify firmware or host changes.
disable-model-invocation: true
---

# Test workflow

Follow the steps in order. Reference: `Testing/testing.md` (test levels, checklist, bug log, workbench rules).

**Hard rules**
* **Never commit, stage, push or amend.** No `git add` / `git commit` / `git push`. Leave all file changes in the working tree for the user.
* Never assume the workbench is available: ask (step 1).
* Do not modify firmware or host source to make a test pass. Only report. (Fixing is a separate request.)
* Use `AskUserQuestion` for every question below; ask all independent questions in one call.

## 1. Ask what to run (every run)

Ask in a single `AskUserQuestion` call:
1. **Scope** (single select): Firmware / Host software / All.
2. **Security tests** (single select): include / skip. Security tests are listed in `Testing/testing.md` section 4. Only run them against the user's own device or the simulator, never other hosts.
3. **Workbench** (single select): not connected (no hardware) / board only (serial) / board + camera.
   If the scope includes firmware and a board is connected, also ask **which COM port** (free text via "Other"; default suggestion: the port in `Testing/testing.md`, currently COM11). Don't flash or open a port before the user answers.

Consequences:
* No hardware: skip every on-device case; mark them `SKIPPED (no hardware)` in the report. Host software tests still run against `Host/tools/sim_device.py` (via `proto_test.py`).
* Board only: no camera cases; verify LED behaviour only through CLI / serial / `GET_LIVE` / the UDP live stream.
* Network: the device is WiFi station only (no soft-AP). It must already have home WiFi credentials (`Scripts/flash_settings.sh`, or Debug > Settings with debug firmware). Never change the PC's network. If the device is not reachable, ask the user before rewriting its settings partition.

## 2. Sync test cases with the code

Before running, check whether features changed since the test cases were last updated:
1. Read `Testing/testing.md` sections 3-4 and note the `Last synced commit:` line at the top of section 3.
2. `git log --oneline <last synced>..HEAD` and `git diff --stat <last synced>..HEAD` (read-only git is fine). If no marker, compare against the full CLI table in `App/software_communication/cli_commands.c`, the command list in `Docs/protocol.md`, algorithms in `App/audio_processing/algorithms.c`, patterns in `Interface/led_patterns`, settings keys, and the views of the host app `Host/ledapp.py`.
3. For every new or changed CLI command, protocol command, algorithm, stage, parameter, setting, pattern, error code or host app view: add or update a row in the checklist (`Testing/testing.md` section 3), and add a native check to `UnitTest/host/test_host.c` (and the stage list in `Scripts/run_host_tests.sh`) for any new pure-C stage or DSP function; add protocol checks to `Host/tools/proto_test.py` (and the simulator `Host/tools/sim_device.py` if needed) for new or changed commands.
4. Update the `Last synced commit:` line to the current `git rev-parse --short HEAD`. Tell the user which cases were added or changed.

## 3. Unit tests (scope: firmware or all)

1. `Scripts/run_host_tests.sh`. If it fails to compile or any check fails:
   * Find the root cause: read the failing check in `test_host.c`, the code it exercises, and `git log` for recent changes to those files. Decide: code regression, test expectation outdated, or test environment (e.g. gcc missing).
   * Write `UnitTest/failures/<YYYY-MM-DD>_<short-name>.md` with: failing check name and verbatim output, expected vs actual, root cause analysis (file:line), suggested fix, whether it is a regression (and the commit that likely caused it). One file per distinct failure.
2. If gcc is missing, report that as the failure cause instead of skipping silently.

## 4. Build checks (scope: firmware or all)

`Scripts/build_debug.sh` and `Scripts/build_release.sh` (can take minutes; run with a long timeout). A build error or new warning is an issue (step 8). Record binary sizes (last known: debug ~906 KB, release ~884 KB); images are also copied to `dist/debug/` and `dist/release/`.

## 5. Firmware tests on device (scope: firmware or all; needs board)

Only with the board connected and the COM port given.
1. Flash: `Scripts/build_debug.sh flash <COMx>` (debug build; the checklist needs the CLI). Flash only after the user chose firmware / all and a port. Do not run `erase-flash` or `flash_settings` without asking (they wipe calibration and/or the WiFi credentials).
2. Capture the boot log over serial (pyserial, 115200, reset via RTS) and grep for `E (`, `panic`, `abort`, `Guru Meditation`, `error`. Any hit is an issue.
3. Run the checklist rows in `Testing/testing.md` section 3 that apply to the workbench level. Drive the CLI over serial or through the protocol (`CLI_EXEC` 0x40 via `Host/ledhost/protocol.py`). With a camera: `ffmpeg -f dshow -i video="<camera>" -frames:v 1 shot.jpg` (ffmpeg at `C:\ffmpeg\bin`) and check LED colours / positions. Rows needing sound, a camera or a human observation that are not available: `SKIPPED (<reason>)`.
4. Rows marked "not yet tested" in the checklist should be run when possible, then the marker removed or kept with the result.
5. Test destructive rows (calibration save, firmware update over WiFi, calib region upload) only after asking the user; restore the original values afterwards.

## 6. Host software tests (scope: host or all)

1. Run `python Host/tools/proto_test.py`. It starts and stops the simulator itself (ports 14210/14211, debug then release build) and runs all checks (framing, live stream, calibration, settings, firmware upload, release refusals).
2. With a board on the home network: write a short Python script in the scratchpad using `Host/ledhost/protocol.py` against the device IP (TCP 4210) and check info, `GET_LIVE`, `LIVE_SUBSCRIBE` (frame rate, seq gaps, unsubscribe), algorithm select, parameters, settings read (debug build), `CLI_EXEC`, and UDP discovery (4211). Do not write settings or start a firmware update without asking.
3. The host app (`Host/ledapp.py`) is a tkinter desktop app; no browser automation applies. Only start it if the user asks, and close it afterwards.
4. Stop any process you started. Never leave stray simulators or scripts running.

## 7. Security tests (only if the user asked)

Run the cases in `Testing/testing.md` section 4 against the device / simulator the user owns. Examples: malformed and oversized protocol frames, bad CRC and length fields, unknown commands, fuzzed payloads, flooding the TCP port, unauthenticated OTA and settings writes, `LIVE_SUBSCRIBE` abuse (rate, port, lease), release-build refusals of debug commands, secrets in logs. Keep tests non-destructive (no sustained DoS beyond a short bounded burst, no actions on other hosts). After fuzzing, confirm the device still responds and did not reboot (check `info` uptime / serial log). Findings go to step 8, with a severity (High / Medium / Low / Info).

## 8. Report issues

Any bug, crash, reboot, hang, wrong behaviour or security finding goes in `Testing/issues_found.md` (create it with a header and a table of contents if missing; append, never overwrite earlier entries). One entry per issue:

```
## ISSUE-<nnn>: <short title>
* Date / firmware version (`info`) / build (debug|release) / scope (firmware|host|security)
* Severity: Critical | High | Medium | Low
* **Expected result:** ...
* **Actual result:** ...
* **Steps performed:** numbered, exact commands / UI actions / inputs
* **Reproducible:** Yes (n of n tries) | Intermittent (x of n) | No (once) - re-run the steps at least twice before deciding
* Evidence: serial log excerpt, command output, screenshot path
* Suspected cause: (optional, file:line)
* Status: Open
```

Do not fix issues in this workflow. Do not add them to the Bug log in `testing.md` (that is for fixed bugs).

## 9. Wrap up

1. Clean up: stop simulators / scripts / serial sessions you started; remove temp files outside the scratchpad.
2. Update `Testing/testing.md`: checklist results are not stored there, but update the `(not yet tested)` markers and the `Last synced commit:` line.
3. Summarise to the user: scope and options chosen, counts of PASS / FAIL / SKIPPED, links to new files in `UnitTest/failures/` and `Testing/issues_found.md`, test cases added in step 2. State clearly what was skipped and why.
4. Remind the user that nothing was committed and list the changed files (`git status --short`).
