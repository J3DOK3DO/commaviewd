# Drive stats and vehicle location from what the comma already keeps

## Rule

No new subscribers, and nothing new running between drives. Everything comes from what openpilot
and sunnypilot already write, read on demand or once after a drive.

## Drive stats

- **When a drive happens:** the control service polls the onroad flag it already reads (`IsOffroad`,
  then `IsOnroad`) every 5 s and notes each drive's start, end and route (`CurrentRoute`, the latest
  new name during the drive; it is cleared as each drive starts).
- **What the drive was:** 30 s after it ends, the control service runs
  `commaview_drive_stats.py after-drive`, a short script in openpilot's own Python at nice 19. It
  reads the drive's own logs (`/data/media/0/realdata/<route>--<n>/qlog.zst`, which hold `carState`
  at 10 Hz and GPS at 1 Hz) for its distance, and keeps a drive list for 62 days
  (`data/drives.json`). Phones group the drives into their own days.
- **comma's totals:** past week and all time, from sunnypilot's own cached copy
  (`ApiCache_DriveStats`) when it is under an hour old, otherwise one request to
  `v1.1/devices/<dongle>/stats` made with the comma's own identity (`openpilot.common.api`), as
  sunnypilot's Trips page does. Refreshed after each drive, every 6 h while parked, and when a phone
  looks at totals older than 30 min (at most every 5 min). Kept through failures, with the error.

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
  ends (`data/location-last.json`); otherwise the log's last fix, or sunnypilot's
  `LastGPSPositionLLK`.
- **Turning it off** deletes the stored positions at once, and nothing is served or saved again
  until it is back on. Positions are never logged or included in support bundles.

## Keeping it honest

`upstream-interface-guard.sh` checks the GPS services and fields, and msgq's ring framing (the size
tag, the `-1` wrap tag, 8-byte alignment, and the write pointer moved after the copy). If upstream
changes any of them the canaries fail, and until the runtime is updated live location falls back to
the log.
