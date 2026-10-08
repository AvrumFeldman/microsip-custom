# Agent instructions

## Coordinate and review

The maintainer prefers parallel delegation for substantial tasks. The primary agent receives the request, assigns independent tasks with clear file ownership, manages dependencies, reviews the changes, and consolidates and validates the result. Use subagent tools first; additional Codex CLI agents are permitted when useful. Avoid redundant work and overlapping uncoordinated edits. Small tasks may be handled directly. Keep the maintainer informed and continue work already authorized.

## Project expectations

- Preserve upstream attribution, licensing notices, and the distinction between this custom fork and official MicroSIP.
- Build the voice-only custom edition with `build-custom.ps1`; `custom.vcxproj` is the supported application project. See README.md and CUSTOM-BUILD.txt for dependencies and behavior.
- Microphone capture must remain closed during idle keypad entry and unanswered incoming ringing, including early media. Restore audio after the last call ends; do not let a pending SIP acknowledgment retain capture or app muting.
- Keep conference participant controls and attended transfers usable in single-call mode.
- Validate changes with isolated local SIP peers and dedicated audio test sessions. Never use a maintainer's real SIP credentials, place outside calls, or mutate the installed MicroSIP configuration for tests.
- Keep generated files in ignored build/output directories. Public packages must use fresh staging, a clean portable configuration, license notices, and matching source; never ship account settings, history, recordings, logs, or credentials.
- Public pull requests are welcome and appreciated, including AI-assisted and AI-generated contributions. Apply the same review and validation standards to all submissions. Document changes relative to official MicroSIP and check the integrated release build before publishing a requested release.
