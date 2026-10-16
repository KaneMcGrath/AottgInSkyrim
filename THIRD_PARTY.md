# Third-party references

The repository's GPL-3.0-only license applies to original AottgInSkyrim code,
not to the third-party components or extracted game assets described here.
All extracted game assets under `assets/hero/` and `assets/ui/`, including
models, animations, textures, audio, fonts and sprites, retain their original
ownership and licensing. Their inclusion does not grant a new license.

`third_party/hde64/` contains the unchanged x64 instruction-length decoder
(`hde64.c`, `hde64.h`, `table64.h`, `pstdint.h`) from MinHook v1.3.4:
https://github.com/TsudaKageyu/minhook/tree/v1.3.4/src/hde . HDE64 is copyright
2008-2009 Vyacheslav Patkov under the BSD-style terms reproduced in its LICENSE;
the compatibility header also retains Tsuda Kageyu's notice. The notices ship in installable packages; decoder source stays in development snapshots. Only the decoder is linked, not MinHook's
hook installation library.

The unchanged SKSE Menu Framework 3 API header and LGPL-2.1 license are vendored in `third_party/skse-menu-framework/`, from https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API at commit `1dcb70179076aae4ab626f43c5baab2735ca5877`. The header dynamically resolves functions from the separately installed framework DLL, including its ImGui API; no second ImGui library/context is linked. The header and its license are included in development source snapshots; installable packages include the license only. This implementation targets the installed framework 3.18. Registration uses `SetSection` / `AddSectionItem`; key capture uses `AddInputEvent`; gameplay input checks `IsAnyBlockingWindowOpened`. The framework also supplies HUD element registration and bitmap image rendering. Without it, configure settings in the INI; the AoTTG HUD requires the framework.

The Skyrim integration uses CommonLibSSE-NG v11.0.0, DirectXMath and DirectX Tool Kit under their upstream licenses. These dependencies are resolved by the pinned build workflow.

The local SkyCraft source supplied under `tools/SkyrimDev/reference/SkyCraft` informed the PlayerCharacter/PlayerControls vtable slots, camera-update call-site discovery, and guarded post-RenderWorld call site for Skyrim 1.7.104. It is licensed as follows:

MIT License

Copyright (c) 2026 chasmlol

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The AoTTG bitmap UI fonts, sprites and source manifests under `assets/ui/` are extracted original game assets from LibAottg's UI packages. Original ownership and licenses still apply; extraction does not grant a new license. See `LibAottg/docs/DamageHud.md` and `LibAottg/docs/KillInfo.md` for provenance.
