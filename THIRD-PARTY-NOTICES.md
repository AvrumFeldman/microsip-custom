# Licensing and source provenance

MicroSIP Custom is a modified, independently maintained work. Copyright notices in upstream files remain intact; dated modification notices identify custom changes. It is not an official MicroSIP product or an endorsement by any upstream author.

## Project license

The official [MicroSIP source page](https://www.microsip.org/source) describes its GPL release, and its source files expressly grant **GPL version 2 or any later version**. This fork's own code uses that same grant. [PJSIP also grants GPL version 2 or later](https://docs.pjsip.org/en/2.15.1/overview/license_pjsip.html).

The executable distribution chooses **GPLv3** under those later-version grants because OpenSSL 3 uses Apache-2.0. Apache-2.0 is compatible with GPLv3; this is not a claim that it is compatible with GPLv2-only code. See the [Apache Software Foundation's compatibility explanation](https://apache.org/licenses/GPL-compatibility.html). `COPYING` contains GPLv3; `LICENSE` preserves the upstream GPLv2 text. Component licenses remain in force for their respective code.

Each binary release includes copyright/license notices, a source link and exact revision, and has matching application-source and dependency-source assets available at the same location. Both source assets are needed. They contain build recipes, configuration, patches, full dependency sources and WAV assets; see [DEPENDENCY-SOURCES.txt](DEPENDENCY-SOURCES.txt). Visual Studio, its ordinary runtime/SDK libraries, Windows, Git and other general build tools are obtained separately.

## Component inventory

| Component | Version/source | License/notice |
| --- | --- | --- |
| MicroSIP | Official 3.22.16 source archive | GPL-2.0-or-later; Copyright 2011–2026 MicroSIP; upstream source headers and `LICENSE` |
| Custom call/audio changes and helper | This release's source revision | GPL-2.0-or-later; combined distribution GPLv3 |
| PJSIP/PJMEDIA/PJLIB/PJNATH | 2.15.1, commit `7de6e686fee1642f5443a3f39f0e5ad5e478a117` | GPL-2.0-or-later with PJSIP's documented exceptions; `licenses/PJSIP.txt` in binary package |
| Opus | 1.6.1 | BSD-style notices and patent grants; `licenses/opus.txt` |
| OpenSSL | 3.6.5 | Apache-2.0; `licenses/openssl.txt` |
| SQLite | 3.53.4 | Public domain; `licenses/sqlite3.txt` |
| SQLiteCpp | 3.3.3 | MIT; `licenses/SQLiteCpp.txt` |
| JsonCpp bundled by MicroSIP | Upstream bundled source | Public domain/MIT option; `licenses/JsonCpp.txt` |
| HIDAPI bundled by MicroSIP | Alan Ott / Signal 11 implementation | BSD alternative selected; `licenses/HIDAPI-BSD.txt`; source preserves original 2009 notice |
| GSM 06.10 | PJSIP bundled 1.0.12 | Jutta Degener/Carsten Bormann permissive notice; `licenses/pjsip/third_party/gsm/COPYRIGHT` |
| Speex | PJSIP bundled source | BSD-style notices; `licenses/pjsip/third_party/speex/COPYING` |
| libSRTP | PJSIP bundled 2.5.0 | BSD-3-Clause; `licenses/pjsip/third_party/srtp/LICENSE` |
| Resampler | PJSIP bundled resample 1.7 | LGPL-2.1; `licenses/pjsip/third_party/resample/COPYING`; complete sources supplied for rebuilding/relinking |
| WebRTC echo cancellation | PJSIP bundled source | BSD-style and component-specific notices/patent grants; `licenses/pjsip/third_party/webrtc/` |
| iLBC | PJSIP bundled source | PJSIP's published iLBC licensing notice; `licenses/iLBC.txt`; Internet Society notices remain in source |
| StdioFileEx / goodgets / VisualStylesXP | MicroSIP bundled helpers | Permissive/public-domain notices retained in source and `licenses/Embedded-helpers.txt` |
| Standard WAV sounds | Official MicroSIP 3.22.16 portable package | Included unchanged with the MicroSIP notices; same WAV files supplied in application-source archive |

PJSIP contains additional platform utilities and optional libraries. Their notices and source remain in the dependency bundle where applicable; inclusion there does not mean the voice-only Windows binary links every optional component. PJSIP's own [third-party licensing inventory](https://docs.pjsip.org/en/2.15.1/overview/license_3rd_party.html) documents its integrated algorithms, notices and build switches.

The build does **not** enable the separately licensed G.722.1 codec. Its unused aggregate build reference is removed, and its `third_party/g7221` implementation is omitted from redistributed dependency source archives. Video and external codec backends are not enabled.

## Source cleanup in this fork

The custom release replaces CMarkup with an independently written adapter using Windows MSXML6, replaces the bundled ATL regex helper, and removes unused MessageBoxX files. These original implementations are not used by the custom build or included in its release-source archives.

The historical source-baseline commit preserves `atlrx.h` under Microsoft's separately scoped Ms-LPL terms, with the full text in `licenses/ATL-Ms-LPL.txt`. Microsoft explicitly included that file in its [ATL Server shared-source release](https://devblogs.microsoft.com/cppblog/atl-server-visual-c-shared-source-software/). Source redistribution under that license is distinct from compatibility with the custom GPL binary; the custom commit deletes the original helper and uses its independent replacement. The retained Ms-LPL notice documents historical source only and does not apply to the replacement.

The baseline omits the original CMarkup and MessageBoxX files because their source-redistribution conditions have not been established for the exact bundled copies. CMarkup's notice requires written permission. MessageBoxX permits author-released source versions, but the bundled copy has not been verified as such a version. These are this fork's documented redistribution choices, not a finding about permissions held by MicroSIP. [UPSTREAM-SOURCE.md](UPSTREAM-SOURCE.md) records the precise baseline, omissions and added notices. Upstream GPL labels are not treated as overriding component-specific terms.

MicroSIP's remaining application code and helpers are preserved under their upstream notices. `lib/Hid.cpp` and `lib/Hid.h` carry MicroSIP's explicit GPL-2.0-or-later grant; the separate `hid.c`/`hidapi.h` license is documented above. The bundled `CSVFile` and `Crypto` files have no separate restrictive license notice in the official source archive.

## Exact inputs

- Source mirror used initially: [swc188/microsip](https://github.com/swc188/microsip), commit `f7fa2f5`.
- Official source: [MicroSIP-3.22.16-src.7z](https://www.microsip.org/download/MicroSIP-3.22.16-src.7z), SHA256 `9c1ff942ae1ae56bdfcdd04c3d6e242a056d7f0dd50bb19f6e13226e7c819b83`.
- Official sound input: [MicroSIP-3.22.16.zip](https://www.microsip.org/download/MicroSIP-3.22.16.zip), SHA256 `b269465205df18de018c2d78ca9d1107b396460e1e8d257c443e75fe99be42f4`. Only WAV files are copied.
- Build recipes and library patches: [vcpkg](https://github.com/microsoft/vcpkg), commit `2750401336fb7c95f6619657a46a7e798661341c`.
- [config/dependencies.json](config/dependencies.json) records exact source archive URLs and checksums. PJSIP configuration and the one aggregate-project edit are controlled by this repository's build script.

HIDAPI's missing bundled license text was restored from its [original author's repository](https://github.com/signal11/hidapi/blob/master/LICENSE-bsd.txt). JsonCpp's notice was restored from its [official repository](https://github.com/open-source-parsers/jsoncpp/blob/master/LICENSE). GPLv3's unchanged text in `COPYING` was obtained from HIDAPI's distributed `LICENSE-gpl3.txt`.
