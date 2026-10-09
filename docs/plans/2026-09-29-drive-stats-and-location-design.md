# Drive stats and vehicle location from what the comma already keeps

## Rule

No new subscribers, and nothing new running between drives. Everything comes from what openpilot
and sunnypilot already write, read without a reader slot or on demand.

Nothing may wait for "after the drive": many commas lose power the moment the car is switched off.
Until 2026-10 a script read each drive's logs 30 s after it ended, from a queue kept in memory, so
on those commas most drives were never recorded (the leaderboard showed nothing for them).

## Drive stats

- **When a drive happens:** the control service polls the onroad flag it already reads (`IsOffroad`,
  then `IsOnroad`) every second and notes each drive's start, end and route (`CurrentRoute`, the
  latest new name during the drive; it is cleared as each drive starts).
- **What the drive was, while it happens** (`src/drive_ledger.h`): the control service follows
  `carState` (100 Hz) from its msgq ring the way `gps_peek` reads GPS (mapped read-only, no reader
  slot), draining it every 100 ms, and adds up the distance from `vEgo` by the rule the log reading
  used: samples in order, each pair at most 1 s apart joined by their average speed (never below 0),
  longer gaps not counted.
- **Kept as it goes:** each drive is its own file, `data/drives/<startMs>.json` (`startMs`, `endMs`,
  `durationS`, `distanceM`, `route`), written owner-only and flushed to disk before it replaces the
  last version: every 30 s, whenever the car comes to a stop (a car stops before it is switched off,
  so the power going loses nothing), and when the comma sees the drive end. The first 10 s write
  nothing (an ignition blip), nor does a drive before the clock is set (its start is worked out from
  how long it has gone on once the clock is set). A control service that restarts mid-drive carries
  on in the file of the same route if it was written in the last 10 minutes. Files of drives that
  ended over 62 days ago are deleted; `data/drives-since.json` says when the comma started keeping
  them. The single `data/drives.json` earlier runtimes kept is moved into the folder once (and kept
  aside as `drives.json.moved`).
- **For the phone** (`/commaview/drive-stats`): the folder as one drive list (`ledger`), without the
  drive going on, which is `current` until it ends. Phones group the drives into their own days.
- **comma's totals:** past week and all time, from sunnypilot's own cached copy
  (`ApiCache_DriveStats`) when it is under an hour old, otherwise one request to
  `v1.1/devices/<dongle>/stats` made with the comma's own identity (`openpilot.common.api`), as
  sunnypilot's Trips page does (`commaview_drive_stats.py totals`, a short run in openpilot's own
  Python at nice 19). Refreshed 90 s after the control service starts and every 6 h after that,
  parked or driving (a comma that powers off with the car is never parked for long; the request to
  comma itself is made at most every 6 h), and when a phone looks at totals older than 30 min while
  parked (at most every 5 min). Kept through failures, with the error.

## Location (off until turned on, per comma)

- **Live, while driving:** prefer the comma's own newest GPS fix, with its acquisition timestamp
  and horizontal accuracy, read straight from openpilot's msgq ring (`/dev/shm/msgq_gpsLocationExternal`,
  `/dev/shm/msgq_gpsLocation`) by `src/gps_peek.cpp`:
  - the ring is mapped read-only and never written: no reader slot, no read pointer, no signal, so
    openpilot's publisher and subscribers cannot tell it is there;
  - each read walks the current lap from its start to the write pointer (msgq moves the pointer only
    after a message is whole), copies only the newest message, and drops it if the writer could have
    come round the ring meanwhile;
  - it runs only when a phone asks (about once a second) and every 5 s during a drive while location
    is on: microseconds each time.
  If GPS is unavailable, sunnypilot's saved position from memory params
  (`/dev/shm/params/d/LastGPSPosition`) is a labelled fallback, dated by file write time. Its writer
  can refresh old coordinates even when localization is invalid, so this timestamp cannot prove
  GPS freshness. Consumers such as app speed-limit lookup must accept only the timestamped
  `comma` GPS source and apply their own age/accuracy bounds. Repeated reads never change the
  GPS acquisition timestamp. If neither source answers, the drive's last finished log minute
  (`commaview_drive_stats.py position`), about a minute behind.
- **Where the last drive ended:** the comma's last fix, saved by the control service when the drive
  ends (`data/location-last.json`); otherwise sunnypilot's `LastGPSPositionLLK`.
- **Turning it off** deletes the stored positions at once, and nothing is served or saved again
  until it is back on. Positions are never logged or included in support bundles.

## Keeping it honest

`upstream-interface-guard.sh` checks the GPS services and fields, `carState.vEgo`, and msgq's ring
framing (the size tag, the `-1` wrap tag, 8-byte alignment, and the write pointer moved after the
copy). If upstream changes any of them the canaries fail, and until the runtime is updated live
location falls back to the log.
