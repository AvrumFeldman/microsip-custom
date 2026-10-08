# MicroSIP Custom

An independently maintained Windows voice-call fork of [MicroSIP](https://www.microsip.org/), focused on Bluetooth microphone ownership, optional music muting, and call controls. This is **not an official MicroSIP release**.

**[Download releases](https://github.com/AvrumFeldman/microsip-custom/releases)** · [Contribute a pull request](CONTRIBUTING.md) · [Build/license details](THIRD-PARTY-NOTICES.md)

## Changes from the official source

| Area | Custom behavior |
| --- | --- |
| Entering digits | Local keypad tones use playback only; entering a number does not open the microphone. |
| Incoming hunt-group calls | Ringing uses playback only; pre-answer media cannot activate capture. Other apps remain unmuted until you answer. |
| Microphone cleanup | Capture follows active call ownership and is released when the final call ends, including local hangup before the SIP peer acknowledges. A periodic state check recovers stale audio state. |
| Music during calls | Optional settings: leave other audio unchanged, mute all other apps, or mute selected executable names. Previous mute states are restored after the last call. |
| Crash recovery | A separate audio guard restores other apps if MicroSIP exits or crashes. |
| Conferences in single-call mode | The **CONF** menu lets you add participants or remove one participant without turning off single-call mode. |
| Transfers | The **Transfer** split-button menu offers blind transfer and attended transfer with a consultation call, completion and cancellation. |
| Call-focused dialer | A full-width Call button and no messaging/video quick buttons in this build. The number field stays below the tabs when resized. Messaging and video source remains available for other builds. |
| Taskbar icon | A stable Windows application ID and explicit window/class icons distinguish the custom application. |
| Call window titles | Name/number separators display correctly in the separate call window used outside single-call mode. |
| Redistributable source | Restricted bundled XML/regex helper code was replaced, an unused message-box helper removed, and source/license packaging added. |

The source baseline is MicroSIP **3.22.16**, the published archive available when this fork was prepared on 2026-10-08. The newer official **3.22.18 binary** was not the source baseline. This build supports **voice only**, **WAV recording**, and PJSIP's available audio codecs plus Opus. It does not include video, the official MP3 recorder, or separately supplied G.729/AMR/SILK/G.722.1 codecs. TLS and SRTP/DTLS are enabled. Windows executables are unsigned.

MP3 is a current build limitation, not a claim that MP3 source is unavailable: PJSIP includes a public legacy MP3 writer, but its stock `pjsua_recorder_create` path does not connect it to an encoder. This build uses the working WAV path; MP3 encoder integration and recording tests are still needed.

## Run and configure

Download `MicroSIP-Custom-<version>.zip`, extract it into its own writable folder, and run `microsip.exe`. Keep `MicroSIPAudioGuard.exe` beside it. The generated account-free `microsip.ini` makes the package portable; your original installation is not replaced. Close the other MicroSIP instance before starting this one.

Add your SIP account and re-enter its password. The public source does not contain the official build's password encryption key, so saved official passwords cannot be imported directly. Contacts can be exported from the original application and imported into this build.

In **Menu → Settings → Other audio during calls**, choose:

- **Leave other audio unchanged** (default).
- **Mute all other apps**.
- **Mute selected apps**, with names such as `Spotify.exe; vlc.exe`. **Add app...** selects an executable. Matching is case-insensitive; a browser executable affects all of that browser's audio sessions, not one tab.

Muting starts on an outgoing call attempt or when you answer an incoming call. It remains active through hold and overlapping calls until the last call ends. It preserves volume levels and apps that were already muted. Manually unmuting an app takes precedence for that session until the next call. New sessions/devices are checked every 200 ms, so a new player may be briefly audible. MicroSIP and Windows system sounds are excluded. The guard cannot restore audio if it is itself killed; exclusive-mode players may not follow Windows session mute controls.

For an attended transfer, choose **Transfer → Attended Transfer (consult first)**, enter the destination and confirm. The original party is held while you speak to the destination. Choose **Complete attended transfer** to connect them, or **Cancel consultation and return** to resume the original call. This uses SIP REFER with Replaces and needs PBX/provider support. Existing configured PBX transfer feature codes remain supported.

During a conference, choose **CONF → Remove participant**, then the party to disconnect. The remaining parties stay connected and single-call mode stays enabled.

## Build and test

Install PowerShell 7, Git, Visual Studio 2022 C++ x86/x64 tools, MFC/ATL, and a Windows SDK. From a writable checkout:

```powershell
pwsh ./build-custom.ps1
# After the pinned dependencies have been built:
pwsh ./build-custom.ps1 -SkipDependencies
```

The script builds the pinned PJSIP/vcpkg dependencies in `../microsip-build`. Output is `out/microsip.exe` and `out/MicroSIPAudioGuard.exe`. `custom.vcxproj` is the supported application project; the original upstream project is retained for reference. See [DEPENDENCY-SOURCES.txt](DEPENDENCY-SOURCES.txt) to rebuild using the downloadable corresponding-source archives.

The regression tests include `tests/MarkupTests.vcxproj` for XML compatibility, `tests/DialPlanTests.vcxproj` for dial-plan parsing, `tests/AudioFocusTests.vcxproj` for real Windows session mute/restore behavior, `tests/smoke_calls.py` for isolated local SIP call/audio transitions, and `tests/smoke_call_controls.py` for transfer/conference behavior. `tests/inspect_title.ps1` checks the actual Unicode title of the separate call window outside single-call mode. Build the C++ tests in Release/Win32 and run their executables in `out`. Audio tests require a Windows render device and target only their own silent sessions. SIP tests use synthetic peers and must run against a separate account-free test instance; see the scripts' prerequisites. Do not run them against a configured production softphone.

Local synthetic SIP and Windows audio-session tests cover software transitions. Actual Bluetooth profile behavior and provider/PBX transfer interoperability still need a listening/call check with your setup.

GitHub Actions builds public pull requests, runs hardware-independent regression tests, and creates clean portable/source artifacts. Version tags trigger release publication with the binary, application sources, dependency sources, and SHA256 checksums together. To package locally, commit the reviewed source after a successful build, then run `pwsh ./package-custom.ps1`; it never packages an existing user's portable directory.

## License and provenance

MicroSIP and this fork's own changes use **GPL-2.0-or-later**, as stated in the upstream source headers. The combined binary is distributed under **GPLv3**, exercising that later-version option because it links Apache-2.0 OpenSSL. [COPYING](COPYING) contains GPLv3; the original [LICENSE](LICENSE) contains GPLv2. Third-party components retain their licenses. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses` folder shipped with binaries.

Each release supplies both application and dependency sources beside the binary. Download **both** source ZIPs for complete corresponding source; GitHub's automatic source ZIP alone does not include external dependencies or WAV assets. See [SOURCE-OFFER.txt](SOURCE-OFFER.txt). There is no warranty, and this project is not endorsed by the MicroSIP team.

Work began from the [swc188/microsip mirror](https://github.com/swc188/microsip) at `f7fa2f5`, followed by the official [3.22.16 source archive](https://www.microsip.org/download/MicroSIP-3.22.16-src.7z), SHA256 `9c1ff942ae1ae56bdfcdd04c3d6e242a056d7f0dd50bb19f6e13226e7c819b83`.

As of 2026-10-08, MicroSIP's [official source page](https://www.microsip.org/source) publishes versioned archives; we have not identified an official public Git repository containing its development history. The unofficial mirror's six commits contain a 3.22.3 snapshot import, four build-workflow edits and one empty commit, not the original MicroSIP development history.

Our history starts with a source-baseline import tagged [`upstream-source-3.22.16`](https://github.com/AvrumFeldman/microsip-custom/tree/upstream-source-3.22.16), followed by one commit containing the initial custom changes. [Compare the baseline with this fork](https://github.com/AvrumFeldman/microsip-custom/compare/upstream-source-3.22.16...main). [UPSTREAM-SOURCE.md](UPSTREAM-SOURCE.md) records the archive checksum, any omitted third-party files and added license notices; the baseline is an imported source snapshot, not a reconstruction of upstream development commits. Upstream attribution and applicable license notices are preserved. This repository is a source-derived fork, not a GitHub fork-network mirror.
