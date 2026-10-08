# Official MicroSIP source baseline

This repository begins with an import of the official **MicroSIP 3.22.16** source archive. The import was made by this fork's maintainer; it is not an original MicroSIP developer commit or a reconstruction of private development history. The next commit contains the custom fork's changes.

- Publisher: [MicroSIP](https://www.microsip.org/).
- Official source index: <https://www.microsip.org/source>.
- Archive: <https://www.microsip.org/download/MicroSIP-3.22.16-src.7z>.
- Archive SHA-256: `9c1ff942ae1ae56bdfcdd04c3d6e242a056d7f0dd50bb19f6e13226e7c819b83`.
- Archive size: 435,979 bytes.
- Original archive: 231 files under `MicroSIP-3.22.16-src/`.
- Imported source: 227 files, preserving archive paths, names, and file contents after removing the containing directory and omitting the four files below.

The official source index publishes release archives. No official public Git repository containing MicroSIP's development history was identified when preparing this import. The independent [swc188 mirror](https://github.com/swc188/microsip) used during initial development is not an ancestor of this import: it contains a 3.22.3 snapshot and build-workflow changes, not original MicroSIP development history.

## Four deliberately omitted files

These omissions avoid republishing third-party source whose permission for this distribution could not be established. They are not presented as unmodified official source.

| Archive path | Reason for omission |
| --- | --- |
| `lib/Markup.cpp` | First Objective Software's CMarkup 6.5 Lite notice requires written permission to redistribute source; such permission has not been established for this fork. |
| `lib/Markup.h` | Same CMarkup notice and source-redistribution restriction. |
| `lib/MessageBoxX.cpp` | PJ Naughter's notice permits source redistribution only of versions released by the author. The exact bundled copy has not been verified as an author-released version, and its restrictions do not grant general redistribution of modified source. |
| `lib/MessageBoxX.h` | Same author-release limitation as `MessageBoxX.cpp`. |

The notices in these exact archive files are the primary evidence. Relevant author information is also available from [First Objective Software](https://www.firstobject.com/guidelines.htm) and [PJ Naughter](https://www.naughter.com/messageboxx.html). This is a description of this fork's cautious redistribution choices under uncertain permissions, not a legal verdict or a claim about permissions held by MicroSIP or other distributors.

MessageBoxX's license does allow source redistribution of author-released versions. The bundled files identify the 2020 version, while the author's available download is version 1.16 from 2022. The files differ, and we did not locate the author's original 2020 archive to establish that the bundled copy satisfies that condition. We therefore omit those two files; we do not claim that all redistribution of MessageBoxX source is prohibited.

The baseline retains references to the omitted files in the original project and source files. It is a comparison snapshot, not a supported build. The following custom commit replaces CMarkup and ATL regular-expression functionality with independently implemented code and removes unused MessageBoxX references.

## ATL regular expressions: retained under its own license

The original `atlrx.h` is included, unchanged. Microsoft [explicitly released this header as part of ATL Server](https://devblogs.microsoft.com/cppblog/atl-server-visual-c-shared-source-software/) under the Microsoft Limited Permissive License (Ms-LPL), which permits source redistribution and modifications subject to its terms, including the Microsoft Windows platform limitation. The Microsoft copyright and attribution notices are retained, and the full license is supplied in `licenses/ATL-Ms-LPL.txt`.

The original archive's `atlrx.h` has SHA-256 `8c9f20338b863623f847ca6371b3a2a3bc018b344142292bb1f7b3a4bc7fab30`. It matches the [CodePlex-preserved ATL Server header](https://github.com/gabegundy/atlserver/blob/5cf8aaead2883b223622300115b77851aff9a467/include/atlrx.h) byte for byte after removing only the four-line Windows CE exclusion guard. That repository is a community preservation, not an official Microsoft Git repository; the license grant is supported by Microsoft's own announcement.

This header keeps its component-specific Ms-LPL terms; it is not relicensed as GPL. The baseline collects source components for comparison. It makes no claim that combining the Ms-LPL component into a GPL binary is permitted. The custom commit removes the header and the custom release does not incorporate it.

## Added license and provenance material

The official archive does not include standalone license texts. This import adds `LICENSE` (the GNU GPL version 2 text), `licenses/HIDAPI-BSD.txt`, `licenses/JsonCpp.txt`, `licenses/ATL-Ms-LPL.txt`, and this provenance document. These additions are clearly separate from the 227 original files. Existing copyright and license notices inside those original files are retained unchanged. Component-specific licenses apply where stated; `LICENSE` does not relicense every component as GPL.

MicroSIP's original source headers permit GNU GPL version 2 or any later version. The embedded HIDAPI files offer a BSD license option; its license text is restored from [the author's HIDAPI repository](https://github.com/signal11/hidapi/blob/master/LICENSE-bsd.txt). The embedded JsonCpp license text is restored from [JsonCpp's repository](https://github.com/open-source-parsers/jsoncpp/blob/master/LICENSE). The additional licenses and corresponding-source details needed for the custom release are documented in its next commit.
