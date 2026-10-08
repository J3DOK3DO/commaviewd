# Documentation

Start with the guide for the task you want to complete. The guides below describe the
current source on `main`; an older installed release may have fewer capabilities.

## Using CommaView

| I want to… | Read |
| --- | --- |
| Understand what runs on the comma | [Project overview](../README.md) |
| Install, update, reinstall, or remove the runtime | [Installation](installation.md) |
| Diagnose a connection, overlay, update, or recording problem | [Troubleshooting](troubleshooting.md) |
| Find the public app setup instructions | [CommaView install guide](https://commaview.com/docs/install) |
| See user-facing changes | [What's new](https://commaview.com/whats-new) |

## Maintaining the runtime

| I want to… | Read |
| --- | --- |
| Understand maintenance gates, the boot hook, or UI export repair | [Maintenance](maintenance.md) |
| Look up commands, ports, configuration, or recording behavior | [Runtime reference](runtime-reference.md) |
| Build locally and run verification | [Development](development.md) |
| Publish a runtime or offer an update to compatible apps | [Releases](releases.md) |
| Understand telemetry forwarding | [Telemetry overview](../commaviewd/docs/ai/telemetry-raw-only-readme.md) and [wire contract](../commaviewd/docs/ai/telemetry-raw-only-deep-dive.md) |
| Prepare a drive for an agreed debugging contribution | [Donate a drive](../tools/contribute/DONATE_A_DRIVE.md) |

[AGENTS.md](../AGENTS.md) defines the repository's branch, local verification, and PR rules.
[LICENSE](../LICENSE) and [DISCLAIMER.md](../DISCLAIMER.md) cover the project's terms.

## Plans, reports, and release notes

These documents are kept at their existing paths so previous links and engineering history
remain useful:

- [Plans](plans/): dated designs, implementation plans, and investigation runbooks. A plan
  describes the work proposed at that time; it is not proof that the work shipped.
- [Reports](reports/): verification results and audits for the commits, upstream versions,
  and hardware named in each report. They do not certify later releases.
- [Release notes](release/): user-facing notes for individual tags. The release workflow uses
  `docs/release/<tag>-user-facing.md` when that exact file exists.
- [Onroad UI parity program](../commaviewd/docs/COM-55-onroad-ui-parity-program.md): historical
  phased plan. Its bridge-side telemetry JSON milestone predates the current raw-only path.

For today's setup and release procedures, use the current guides above. When changing
behavior, update the relevant guide and link to supporting evidence rather than adding a
new procedure to the root README.
