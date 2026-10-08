# StarPilot support

Supported remote: StarPilot's GitHub repository, on comma 3X (tizi) and comma four (mici), over
HTTPS or SSH and in any letter case. StarPilot's installer (`installer.comma.ai/firestar5683/StarPilot`)
clones it by its former name, `github.com/firestar5683/openpilot`, which GitHub redirects to
`github.com/firestar5683/StarPilot`; both are recognized. The first tested snapshot is
`2a12dbd0ad94b46f8a7b6099d32d7216b7676f79` from the StarPilot branch.

The normal installer identifies StarPilot, installs its dedicated UI exporter and preserves
patch provenance/repair/revert behavior. The runtime reads either road/driver or
narrowRoad/cabin encoder queue names without subscribing or taking a msgq reader slot, and
follows whichever of the two was opened most recently. Wire service IDs are unchanged; StarPilot
UI fields are nested in JSON uiStateOnroad.

For the first alpha test, use runtime `v0.0.58-alpha.1` with Android app
`app-v0.0.162-alpha.1`. The existing beta app does not select alpha runtime offers.

The Android companion needs the matching StarPilot overlay change. It receives explicit
lateral-only/longitudinal-only states, resolved border and screen-edge colors, camera
selection, custom widths and path theme settings. StarPilot shows its driving modes only through
those colors, so no mode text is exported. Existing clients can ignore the added fields.

StarPilot Disable Logging (the `NoLogging` param) stops encoderd, loggerd and the other logging
processes. Turn this setting off to stream video through CommaView; this integration does not
change that setting or the fork's process predicates. StarPilot applies it only while Device
Management is on and outside the `StarPilot-Vetting` branch; the exporter reports the resolved
toggle (`videoDisabledReason`), so the app explains a missing video only when it really applies.

StarPilot's High Bitrate Recording (`/cache/use_HD`) and Konik (`/cache/use_konik`) settings move
drive logs to `/data/media/0/realdata_HD` or `realdata_konik` (system/hardware/hw.py). Recordings,
drive stats and drive retention follow the same rule, and only while StarPilot is installed, since
the markers outlive a fork change.

StarPilot runs its livestream encoder only without a car, so the developer-only
`COMMAVIEW_VIDEO_SOURCE=livestream` has no video in a car. The default encoder source is unaffected.

Local verification: `scripts/ci-local.sh starpilot-pinned-mici starpilot-pinned-tizi`.
The standard `--all` matrix also includes both pinned StarPilot UI layouts. A device test
is still required for camera switching, real encoding/decoding and optional native widgets.
