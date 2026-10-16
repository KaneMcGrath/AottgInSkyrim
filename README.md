# AottgInSkyrim

Skyrim SKSE adapter for the LibAottg native gameplay runtime.

## Development

This repository lives at `SkyrimAottg/` in the LibAottg workspace. Building
currently requires the sibling `../LibAottg/native` source tree and the pinned
dependencies and PowerShell helpers under `../tools/SkyrimDev/`.

Run `../tools/SkyrimDev/Setup.ps1` to prepare dependencies and
`../tools/SkyrimDev/Build-Run.ps1` to build and deploy to the dedicated development
game. The project targets x64 C++23 and CommonLibSSE-NG v11.0.0.

## License

The original AottgInSkyrim source code, build scripts, tests and documentation
in this repository are licensed under the **GNU General Public License,
version 3 only** (`GPL-3.0-only`). See [LICENSE](LICENSE) for the full terms.

Third-party code retains its own licenses and notices. Extracted game assets
under `assets/`, including models, animations, textures, audio and UI assets,
retain their original ownership and licensing; they are not relicensed under
the GPL by this repository. See [THIRD_PARTY.md](THIRD_PARTY.md).

External dependencies, including the sibling LibAottg repository, retain their
own licensing terms.

## Slop Warning

This entire repo was mostly AI generated with codex/gpt-6.1-sol.  I expect it would only be useful to other clankers
