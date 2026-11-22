# MicroSIP Custom

An independently maintained Windows voice-call fork of [MicroSIP](https://www.microsip.org/), focused on Bluetooth microphone ownership, optional music muting, and call controls. This is **not an official MicroSIP release**.

**[Download releases](https://github.com/AvrumFeldman/microsip-custom/releases)** · [Contribute a pull request](CONTRIBUTING.md) · [Build/license details](THIRD-PARTY-NOTICES.md)

Pull requests are welcome and appreciated, including AI-assisted and AI-generated contributions. See [CONTRIBUTING.md](CONTRIBUTING.md) for how to submit a change.

## Changes from the official source

| Area | Custom behavior |
| --- | --- |
| Accounts across builds | Custom builds share a dedicated per-user profile, so a new release reuses your SIP accounts and settings. Existing custom portable data is migrated once; explicit portable mode remains available. |
| Updates | Checks this fork's stable GitHub releases, verifies the download and installs application files after calls end. Account settings and other user data are preserved. |
| SIP reconnection | Registration failures keep retrying after 5–10 seconds; a 25-second watchdog covers failures that PJSIP does not automatically retry. Healthy registrations retain their normal refresh schedule. |
| Incoming call history | Incoming calls are saved even without a display name or when rejected by Do Not Disturb or call limits. |
| Windows startup | If Windows startup points to another MicroSIP copy, the custom app offers to switch that entry to its own executable. |
| Call waiting and second calls | In Single Call Mode, Hold opens a menu to hold or resume, start a new call, or switch between connected calls. Answering a second incoming call holds the first. |
| Entering digits | Local keypad tones use playback only; entering a number does not open the microphone. |
| All incoming calls | Ringing uses playback only; pre-answer media cannot activate capture. Ringing alone does not mute other apps. |
| Incoming-call focus | With bring-to-front enabled, the ringing window appears in front without taking focus from the app you are using or triggering Windows' pointer snap to a dialog button. |
| Microphone cleanup | Capture follows active call ownership and is released when the final call ends, including local hangup before the SIP peer acknowledges. A periodic state check recovers stale audio state. |
| Music during calls | Optional settings: leave other audio unchanged, mute all other apps, or mute selected executable names. Only mutes applied by MicroSIP are restored; pre-muted apps and manual changes are preserved. |
| Bluetooth after hangup | Keep other audio muted until capture closes and Windows reports that the relevant Bluetooth call audio connection has ended. No fixed unmute delay or headset model list. |
| Crash recovery | A separate audio guard retains the same recovery checks if MicroSIP exits or crashes. |
| Conferences in single-call mode | The **CONF** menu lets you add participants or remove one participant without turning off single-call mode. |
| Transfers | The **Transfer** split-button menu offers blind transfer and attended transfer with a consultation call, completion and cancellation. |
| Call-focused dialer | A full-width Call button and no messaging/video quick buttons in this build. The number field stays below the tabs when resized. Messaging and video source remains available for other builds. |
| Taskbar icon | A stable Windows application ID and explicit window/class icons distinguish the custom application. |
| Call window titles | Name/number separators display correctly in the separate call window used outside single-call mode. |
| Redistributable source | Restricted bundled XML/regex helper code was replaced, an unused message-box helper removed, and source/license packaging added. |

The source baseline is MicroSIP **3.22.16**, the published archive available when this fork was prepared on 2026-10-08. The newer official **3.22.18 binary** was not the source baseline. This build supports **voice only**, **WAV recording**, and PJSIP's available audio codecs plus Opus. It does not include video, the official MP3 recorder, or separately supplied G.729/AMR/SILK/G.722.1 codecs. TLS and SRTP/DTLS are enabled. Windows executables are unsigned.

MP3 is a current build limitation, not a claim that MP3 source is unavailable: PJSIP includes a public legacy MP3 writer, but its stock `pjsua_recorder_create` path does not connect it to an encoder. This build uses the working WAV path; MP3 encoder integration and recording tests are still needed.

## Run and configure

**Menu → About** shows the installed custom version and a clickable link to this fork's GitHub repository.

Download `MicroSIP-Custom-<version>.zip`, extract it into a writable folder, and run `microsip.exe`. Keep `MicroSIPAudioGuard.exe` beside it. Custom builds share `%APPDATA%\MicroSIP Custom\microsip.ini`, contacts and call history, with logs and new recordings under `%LOCALAPPDATA%\MicroSIP Custom`. This profile is separate from official MicroSIP. A new custom release reuses it even when the executable is in a different folder; only one running copy may own the same profile.

For the first upgrade from an older custom portable build, close it and put the new application files into that existing custom folder. On launch, accounts, encrypted passwords, contacts and call history are copied into the shared profile without deleting the originals or replacing an existing shared profile. Existing recordings stay in their original directory. Later upgrades reuse the shared profile automatically. Release ZIPs do not contain a live `microsip.ini`, so extracting a new release cannot overwrite your existing accounts.

For a fully portable copy, create an empty `microsip.portable` file beside the executable before launching. That copy uses its adjacent INI and databases. Keep that folder for subsequent upgrades. The in-app updater preserves both portable and shared profiles. The inherited `/reset` and `/resetnoask` commands are disabled for custom builds; back up the selected profile before removing data manually.

To use a separate persistent custom profile, run `microsip.exe /profile-directory "C:\path\to\profile"`. The path must be absolute; this selects shared-profile behavior in that directory without accessing the default custom profile. The selection is preserved by startup registration, crash recovery and the updater. The optional final `/minimized` argument starts an existing profile in the tray.

When moving from official MicroSIP for the first time, add your SIP account and re-enter its password. The public source does not contain the official build's password encryption key, so saved official passwords cannot be imported directly. Contacts can be exported from the original application and imported into this build. Existing custom accounts and passwords are preserved across custom updates.

If your Windows startup entry points to another MicroSIP copy, this version asks whether to use the current custom executable instead. Declining preserves the existing entry. You can enable startup for the current copy in Settings.

In **Menu → Settings → Other audio during calls**, choose:

- **Leave other audio unchanged** (default).
- **Mute all other apps**.
- **Mute selected apps**, with names such as `Spotify.exe; vlc.exe`. **Add app...** selects an executable. Matching is case-insensitive; a browser executable affects all of that browser's audio sessions, not one tab.

**Menu → Check for updates** checks the latest stable release at `AvrumFeldman/microsip-custom` on GitHub. Automatic checks default to daily and use the interval in Settings. The first upgrade switches the old official-update suppression to this GitHub channel once; later choices, including Never, are retained. Checks also run while the app remains open. The custom app does not use the official MicroSIP update service or expire based on its build date.

When a newer release is available, confirm the download and restart. The app verifies the release archive and its file checksums before closing, waits until all calls (including ringing calls) have ended, then replaces application files and restarts in the same folder. It preserves INI files, contacts, databases, recordings and existing sounds. Replacement files are backed up; if installation fails, the updater attempts to restore them and reports the error. A running audio guard is allowed to finish recovery rather than being killed for an update.

Muting starts on an outgoing call attempt or when you answer an incoming call. It remains active through hold and overlapping calls. After the last call ends, capture is closed first. For a Bluetooth Classic call connection, audio stays muted until Windows reports its SCO connection has closed, preventing music from briefly playing at the headset's call-profile volume. The wait follows device state, not a fixed delay. Device identities and connections are discovered through Windows APIs; there are no headset brand or model checks.

The guard changes session mute only, never volume levels. It only unmutes sessions that it muted itself; apps that were already muted stay muted. A manual unmute takes precedence for that session throughout the call and recovery, including a subsequent manual re-mute. A new call during recovery keeps the existing mute ownership without briefly restoring audio. Helpers from restarted copies coordinate their holds so an old helper cannot restore music during a new call. New sessions/devices are checked periodically, so a new player may be briefly audible. MicroSIP and Windows system sounds are excluded.

If Windows cannot identify the relevant transport or confirm that it closed, audio remains muted rather than guessing from elapsed time or the endpoint's advertised format. **Menu → Restore other audio now** explicitly releases MicroSIP's mutes; the independent audio guard also offers manual restoration if the main app has exited. Automatic Bluetooth recovery uses Windows Bluetooth Classic transport notifications. Unresolved devices and LE Audio currently require manual restoration. Wired and USB audio restore after capture closes; a wireless dongle that presents itself only as USB audio hides its internal profile state, so this build cannot wait for that internal transition. The guard cannot restore audio if it is itself killed; exclusive-mode players may not follow Windows session mute controls.

For an attended transfer, choose **Transfer → Attended Transfer (consult first)**, enter the destination and confirm. The original party is held while you speak to the destination. Choose **Complete attended transfer** to connect them, or **Cancel consultation and return** to resume the original call. **End** also cancels the consultation, including while it is ringing. A consultation routed back to the same account has separate Answer/Decline controls; answering it preserves the original held call. This uses SIP REFER with Replaces and needs PBX/provider support. Existing configured PBX transfer feature codes remain supported.

Use the call controls to hang up before exiting. A forced process termination cannot send SIP hangup messages; the remote call may remain until the other party hangs up or the PBX detects the lost session. The audio guard restores Windows audio, but cannot terminate SIP calls after the application has been killed.

During a conference, choose **CONF → Remove participant**, then the party to disconnect. The remaining parties stay connected and single-call mode stays enabled.

Answering a second incoming call in single-call mode holds the current call after the peer acknowledges hold. With multiple connected calls, click **Hold** and choose **Switch to** the party you want to resume; the other call is held first. The menu also offers hold/resume for the current party. If the peer cannot accept hold, the new call is left unanswered and an error is shown.

Choose **Hold → New call…** to dial a second call while keeping the first. **Settings → Hold before a new call** lets you hold when you press **Call** (the default) or as soon as you choose **New call…**. Dialing waits for the original peer to accept hold. MicroSIP resumes a call it held for you when you cancel, the new call fails, or the new call ends, if no other call is active.

## Build and test

Install PowerShell 7, Git, Visual Studio 2022 C++ x86/x64 tools, MFC/ATL, and a Windows SDK. From a writable checkout:

```powershell
pwsh ./build-custom.ps1
# After the pinned dependencies have been built:
pwsh ./build-custom.ps1 -SkipDependencies
# Build elsewhere if the usual output executable is currently running:
pwsh ./build-custom.ps1 -SkipDependencies -OutputDirectory build/candidate
```

The script builds the pinned PJSIP/vcpkg dependencies in `../microsip-build`. Output is `out/microsip.exe` and `out/MicroSIPAudioGuard.exe`. `custom.vcxproj` is the supported application project; the original upstream project is retained for reference. See [DEPENDENCY-SOURCES.txt](DEPENDENCY-SOURCES.txt) to rebuild using the downloadable corresponding-source archives.

The regression tests include `tests/MarkupTests.vcxproj` for XML compatibility, `tests/DialPlanTests.vcxproj` for dial-plan parsing, `tests/BluetoothRecoveryTests.vcxproj` for hardware-independent capture/transport recovery states, `tests/AudioFocusTests.vcxproj` for real Windows session mute/restore behavior, `tests/smoke_calls.py` for isolated local SIP call/audio transitions, `tests/smoke_call_controls.py` for transfer/conference behavior, and `tests/smoke_self_transfer.py` for consultations routed back to the same account. `tests/inspect_title.ps1` checks the actual Unicode title of the separate call window outside single-call mode. Build the C++ tests in Release/Win32 and run their executables in `out`. Audio tests require a Windows render device and target only their own silent sessions. SIP tests use synthetic peers and must run against a separate account-free test instance; see the scripts' prerequisites. Do not run them against a configured production softphone.

Local synthetic SIP and Windows audio-session tests cover software transitions. A hardware check also confirmed that the Windows observer waited for an actual Bluetooth SCO disconnect after capture closed. This does not certify every Windows driver or headset; audio quality and provider/PBX transfer interoperability still need a listening/call check with your setup.

GitHub Actions builds public pull requests, runs hardware-independent regression tests, and creates clean portable/source artifacts. Version tags trigger release publication with the binary, application sources, dependency sources, and SHA256 checksums together. To package locally, commit the reviewed source after a successful build, then run `pwsh ./package-custom.ps1`; it never packages an existing user's portable directory.

`tests/StartupRegistrationTests.vcxproj` verifies startup migration using a temporary registry key. `tests/smoke_registration.py`, `tests/smoke_call_history.py`, and `tests/smoke_call_waiting.py` cover registration recovery, durable incoming history, and call switching with failure recovery. Add `--failure-cases` to the waiting test to exercise a refused resume.

Run UI-driven SIP tests in a separate Windows session or VM. Within that environment, use `tests/run_isolated_desktop.py` to give the test app and driver a dedicated desktop. Prepare a synthetic portable fixture under `build` using the driver's stated settings, then run, for example:

```powershell
python tests/run_isolated_desktop.py --app build/test-fixture/microsip.exe --driver tests/smoke_call_waiting.py --timeout 120 -- --failure-cases
python tests/run_isolated_desktop.py --app build/test-fixture/microsip.exe --driver tests/smoke_call_history.py -- build/test-fixture/call_log.db
```

The Windows desktop used by the harness can still affect focus or show UI in the same session. Use only synthetic loopback accounts in the fixture. The harness checks its configuration before launch.

`tests/CustomProfileTests.vcxproj` verifies shared-profile migration and reuse using temporary directories. `tests/CustomUpdateTests.vcxproj` checks release identity, numeric versions, trusted URLs and checksums. Run `python tests/smoke_custom_update.py` to verify synthetic package installation, user-data preservation, invalid archives and rollback in a disposable installation; it never applies an update to an installed MicroSIP copy.

If built with a separate output directory, pass the matching `-BinaryDirectory build/candidate` to the packaging script. The executable version, custom source version and release tag must agree. Packaging still requires reviewed, committed source and always uses fresh staging without user data.

## License and provenance

MicroSIP and this fork's own changes use **GPL-2.0-or-later**, as stated in the upstream source headers. The combined binary is distributed under **GPLv3**, exercising that later-version option because it links Apache-2.0 OpenSSL. [COPYING](COPYING) contains GPLv3; the original [LICENSE](LICENSE) contains GPLv2. Third-party components retain their licenses. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses` folder shipped with binaries.

Each release supplies both application and dependency sources beside the binary. Download **both** source ZIPs for complete corresponding source; GitHub's automatic source ZIP alone does not include external dependencies or WAV assets. See [SOURCE-OFFER.txt](SOURCE-OFFER.txt). There is no warranty, and this project is not endorsed by the MicroSIP team.

Work began from the [swc188/microsip mirror](https://github.com/swc188/microsip) at `f7fa2f5`, followed by the official [3.22.16 source archive](https://www.microsip.org/download/MicroSIP-3.22.16-src.7z), SHA256 `9c1ff942ae1ae56bdfcdd04c3d6e242a056d7f0dd50bb19f6e13226e7c819b83`.

As of 2026-10-08, MicroSIP's [official source page](https://www.microsip.org/source) publishes versioned archives; we have not identified an official public Git repository containing its development history. The unofficial mirror's six commits contain a 3.22.3 snapshot import, four build-workflow edits and one empty commit, not the original MicroSIP development history.

Our history starts with a source-baseline import tagged [`upstream-source-3.22.16`](https://github.com/AvrumFeldman/microsip-custom/tree/upstream-source-3.22.16), followed by one commit containing the initial custom changes. [Compare the baseline with this fork](https://github.com/AvrumFeldman/microsip-custom/compare/upstream-source-3.22.16...main). [UPSTREAM-SOURCE.md](UPSTREAM-SOURCE.md) records the archive checksum, any omitted third-party files and added license notices; the baseline is an imported source snapshot, not a reconstruction of upstream development commits. Upstream attribution and applicable license notices are preserved. This repository is a source-derived fork, not a GitHub fork-network mirror.
