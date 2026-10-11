# Telemetry wire contract

[Documentation](../../../docs/README.md) · [Telemetry overview](telemetry-raw-only-readme.md) · [Runtime reference](../../../docs/runtime-reference.md)

## Pipeline contract
- Source: the exporter injected into the upstream UI (`comma/src/commaview_export.<flavor>.py`) builds one compact JSON payload per service. It sends each as `[u32 BE length][frame version 1][service_idx][json]` over the Unix socket `/data/commaview/run/ui-export.sock` (`COMMAVIEWD_UI_EXPORT_SOCKET` overrides it).
- Queue policy: the bridge's UI export socket keeps only the latest payload per service. Every 50 ms the telemetry loop forwards payloads that are newer than the last one sent, fresher than 750 ms, and allowed by the per-service runtime-debug policy (`pass`, `sample` at `sampleHz`, or `off`).
- Transport: each forwarded payload goes out as a `MSG_META_RAW` frame. It rides on the road/wide video ports when the client allows it, and always on the telemetry port 8203.
- Consumer: the Android app decodes the JSON payload and drives overlays and fallback views.

## Envelope
`[u32 BE frame length][0x04 MSG_META_RAW][0x05 envelope version][service_idx u8][u32 BE payload length][payload]`

The payload is the exporter's JSON, unchanged. There is no which/logMonoTime header and no typed section. `logMonoTime` travels inside the JSON where a service has one.

## Services
`service_idx` is the position in this list. The order is wire protocol and must match `kTelemetryServices` (bridge_runtime.cc), `telemetry::kDefaultServicePolicies`, the exporters' `*_SERVICE_INDEX` constants, and the app's `TELEMETRY_SERVICE_TYPES`:

0 uiStateOnroad, 1 selfdriveState, 2 carState, 3 controlsState, 4 onroadEvents, 5 driverMonitoringState, 6 driverStateV2, 7 modelV2, 8 radarState, 9 liveCalibration, 10 carOutput, 11 carControl, 12 liveParameters, 13 longitudinalPlan, 14 carParams, 15 deviceState, 16 roadCameraState, 17 pandaStatesSummary, 18 onroadProjection, 19 wideRoadCameraState.

Index 20 is used only between the exporter and the bridge (recording recipe events) and is never forwarded to the app.

## Failure signatures
- If the exporter regresses, `uiExportSocket.connected` is false or `malformedCount` grows, and services stop emitting.
- If the Android decode regresses, overlays are missing even though `emittedCount` keeps growing.

## Troubleshooting order
1. Confirm the bridge startup markers and `UI_SOCKET_PREFERRED=on`.
2. Confirm `/commaview/onroad-ui-export/status` reports the exporter installed (`patchVerified`).
3. Confirm per-service `emittedCount` growth in the runtime stats.
4. Confirm the Android parser receives the expected service indexes.

## Coordinating protocol changes

The exporter, bridge policy, wire service indexes, and Android parser must agree. Describe
the corresponding app/runtime compatibility range when changing the contract, and follow
the [release procedure](../../../docs/releases.md) before offering the runtime to apps.
Coordinate companion changes with the Android maintainers and keep unrelated UI work on
its own branch.
