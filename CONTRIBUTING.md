# Contributing

Public issues and pull requests are welcome. Fork this repository, create a branch and open a pull request against `main`.

Explain the behavior before and after your change and describe relevant testing. Keep microphone ownership, call-state transitions and audio restoration explicit. Ringing without answering must not acquire capture or mute other applications. Do not add tests that call real phone numbers or depend on private SIP credentials.

Build with `pwsh ./build-custom.ps1`. See the README for the Windows toolchain and tests. GitHub Actions builds pull requests with a read-only token. First-time external contributions may need a maintainer to approve the workflow run; release publishing runs only for version tags in this repository.

Never include `microsip.ini`, SIP credentials, personal contacts, call history, recordings, logs or account screenshots in a pull request or release. Use synthetic local test data. Release packaging always starts from a fresh allowlisted staging directory.

Preserve upstream notices and mark modifications to upstream files with a short dated notice. New project code should use `SPDX-License-Identifier: GPL-2.0-or-later`; the combined distribution uses GPLv3 because it links Apache-2.0 OpenSSL. Added dependencies must be compatible with that distribution and have source and notices included in release preparation. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Submitting a contribution means you have the right to contribute it under the applicable project license. No contributor agreement or permission to open a pull request is required.
