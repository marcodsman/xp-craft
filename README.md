# xp-craft

A from-scratch Minecraft-style voxel game for the Windows XP entertainment box
(Acer AO751h: Atom Z520 @ 1.33 GHz single-core, GMA 500 / PowerVR SGX535, 1 GB RAM).
Cross-compiled from Linux, deployed over the SMB share, driven and verified remotely.

**The bet:** the SGX535 is literally the iPhone 3GS GPU, and Minecraft Pocket Edition
shipped on that. A hand-tuned native D3D9 engine at MCPE-0.1 scope can beat the
~13 FPS ceiling a general-purpose engine (Minetest) hits on this hardware.

## Hardware ground rules (measured, 2026-07-03)

Established by running Minetest 0.4.17.1 on the box (see the maintenance repo,
`windows-xp-pc.md` + memory `xp-box-3d-renderer`):

- **Direct3D 9 is the ONLY usable 3D path.** The XP Poulsbo driver's OpenGL is a
  generic GL 1.1 stub (apps crash); D3D9 renders correctly. Fixed-function / SM2.0.
- Reference numbers (Minetest, 800x600, eye-candy off): software renderer ~2 FPS,
  D3D9 view-range 25 → ~9 FPS, view-range 10 → ~13 FPS. Draw distance dominates.
- Target: **beat 13 FPS** with a small hand-rolled engine; 20+ FPS = success.
- RAM budget ~1 GB total, XP itself uses ~150 MB. Small worlds are fine (MCPE had
  256x256x128 worlds on this exact GPU).

## Build / deploy / run

```bash
make            # cross-compile (needs gcc-mingw-w64-i686)
make deploy     # copy build/ onto the box via /media/Acer_Notebook (XP_Share)
make run        # launch on the TV over SSH (xprun) + grab a screenshot (xpshot)
make kill       # stop a running instance (XP locks the exe while running)
```

`deploy`/`run` only work at home (the box is LAN-only; the mount is travel-safe
and just fails fast when away).

## Milestones

- [x] 0. Walking skeleton: window + D3D9 device + clear + one triangle + FPS title
- [ ] 1. Textured cube, fly camera (WASD+mouse), 16-bit backbuffer test
- [ ] 2. One chunk (16x16x16), naive per-face meshing, texture atlas
- [ ] 3. Chunk grid + frustum culling + greedy meshing; measure the FPS/view-range curve
- [ ] 4. Block break/place, raycast, save/load
- [ ] 5. Terrain gen (value noise), day/night, the game part
- [ ] Stretch: gamepad support, launch entry in xp-launcher

## Style

Plain C (C99), COBJMACROS-style D3D9 (no C++), same cross-compile pattern as
`~/projects/personal/xp-launcher` and the proven `tri_d3d9.c` in the maintenance
repo's `xp-gpu-trace/`.
