# CLAUDE.md — xp-craft

From-scratch voxel game (Minecraft clone) for the XP entertainment box. Read README.md
first — it has the measured hardware budget and milestones.

## Hard constraints (do not violate)

- **Target machine:** Acer AO751h — Atom Z520 (single core, in-order, 1.33 GHz),
  GMA 500 (PowerVR SGX535), 1 GB RAM, Windows XP SP3, 32-bit. LAN-only, no internet.
- **Renderer: Direct3D 9 only.** OpenGL on this box is a broken GL 1.1 stub — never
  target it. Fixed-function or SM2.0 max. This was measured, not guessed
  (maintenance repo, memory `xp-box-3d-renderer`).
- **Plain C99 + COBJMACROS D3D9.** No C++, no SDL, no external deps beyond what
  mingw-w64 ships. Keep the exe XP-compatible (mingw-w64 defaults are fine).
- Performance reference: Minetest under D3D9 does ~9-13 FPS here. If a change tanks
  FPS below that, it's a regression against the whole point of the project.

## Workflow

- `make` cross-compiles with `i686-w64-mingw32-gcc`.
- `make deploy` copies to `/media/Acer_Notebook/xp-craft/` (= `C:\XP_Share\xp-craft\`
  on the box — the share IS the box's local disk, no second copy step needed).
- `make run` launches on the box (`~/bin/xprun`) and screenshots (`~/bin/xpshot`) —
  **this is how you verify anything visual; use it after every change that draws.**
  The FPS counter lives in the window title so screenshots capture it.
- The box holds the exe locked while running — `make kill` before redeploying.
- Only works at home. If `/media/Acer_Notebook` fails fast / ping 192.168.0.245 fails,
  the box is unreachable — build only, don't try to deploy.

## Conventions

- Sibling project `~/projects/personal/xp-launcher` (SDL2 big-picture launcher) uses
  the same build/deploy/run pattern — keep the Makefiles feeling identical.
- Proven-on-box D3D9 reference code: `~/projects/personal/maintenance/xp-gpu-trace/tri_d3d9.c`.
- Box operation docs (SSH quirks, screenshots, clock, share):
  `~/projects/personal/maintenance/windows-xp-pc.md`.
