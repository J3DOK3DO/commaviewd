# StarPilot support

Supported remote: https://github.com/firestar5683/StarPilot (HTTPS and SSH), on comma 3X
(tizi) and comma four (mici). The first tested snapshot is
`2a12dbd0ad94b46f8a7b6099d32d7216b7676f79` from the StarPilot branch.

The normal installer identifies StarPilot, installs its dedicated UI exporter and preserves
patch provenance/repair/revert behavior. The runtime reads either road/driver or
narrowRoad/cabin encoder queue names without subscribing or taking a msgq reader slot.
Wire service IDs are unchanged; StarPilot UI fields are nested in JSON uiStateOnroad.

For the first alpha test, use runtime `v0.0.58-alpha.1` with Android app
`app-v0.0.162-alpha.1`. The existing beta app does not select alpha runtime offers.

The Android companion needs the matching StarPilot overlay change. It receives explicit
lateral-only/longitudinal-only states, resolved status colors, conditional modes, camera
selection, custom widths and path theme settings. Existing clients can ignore the added fields.

StarPilot Disable Logging stops encoderd. Turn this setting off to stream video through
CommaView; this integration does not change that setting or the fork's process predicates.

Local verification: `scripts/ci-local.sh starpilot-pinned-mici starpilot-pinned-tizi`.
The standard `--all` matrix also includes both pinned StarPilot UI layouts. A device test
is still required for camera switching, real encoding/decoding and optional native widgets.
