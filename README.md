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

- [x] 0. Walking skeleton: window + D3D9 device + clear + one triangle + FPS title — **177 FPS**
- [x] 1. Textured cube, fly camera (WASD+mouse), depth buffer, procedural texture — **160-164 FPS**
      (16-bit backbuffer test deferred to milestone 3, where fill rate starts to matter)
- [x] 2. One chunk (16x16x16), naive per-face meshing, texture atlas — **106-108 FPS**,
      2172 tris, meshed in 1 ms, static vertex buffer, orbit camera
- [x] 3. Chunk grid + frustum culling + greedy meshing + fog + HW T&L + bench mode.
      **The curve (800x600, 20x20-chunk world, hardware VP):**
      r=32 → 58 FPS (vsync-ish), r=48 → 52, r=64 → 57, r=96 → 38.5, **r=128 → 29 FPS**.
      Greedy = 276 tris/chunk avg (8x under naive). Driver exposes HW T&L and it works.
      Full-world mesh 6.4s (16 ms/chunk) — one-time; make rebuilds async in M4.
      `make bench` runs the scripted benchmark and prints bench.txt from the share.
- [x] 4. Block break/place (LMB/RMB + hold-repeat), Amanatides-Woo raycast picking,
      wireframe highlight, crosshair, planks block (keys 1-4 select), world save/load
      (world.dat, F5 + on exit, loaded at startup). Edit remesh: **4 ms**. Verified
      remotely: placed planks on screen, byte-counted them in world.dat, survived a
      kill+relaunch, broke one and watched the count drop. y=0 is bedrock.
- [x] Stretch: gamepad support (now via the shared xp-pad mapping — real PS buttons)
      and a top-billing entry in xp-launcher's games list.

## The road to a full game

Tick these off as they land. Ordered so the game is playable-feeling early and
world-rich later; every step gets measured on the box before it counts.

**Phase A — feels like a game**
- [x] A1. Gravity, jumping, block collision (walk mode; F / double-tap CROSS toggles fly;
      CROSS/SPACE jumps; axis-separated AABB vs blocks, 0.6x1.8 player box, no
      measurable FPS cost)
- [ ] A2. Hotbar UI with block icons + selection highlight (kill the title-bar readout)
- [ ] A3. Day/night cycle (~10 min loop: sky + fog color + light-level shading)
- [ ] A4. Sounds: dig/place/footsteps/jump (waveOut, tiny generated WAVs)

**Phase B — a world worth exploring**
- [ ] B1. Value-noise terrain with octaves (replace the layered sines)
- [ ] B2. Taller world (raise WORLD_H past 16 — mesher masks are sized CHUNK*CHUNK,
      must grow with it)
- [ ] B3. Trees + wood/leaves/sand block types
- [ ] B4. Water: translucent, non-solid, swimmable-ish (render pass after opaque)
- [ ] B5. Caves (3D noise carve) + stone variety underground

**Phase C — polish**
- [ ] C1. Fullscreen at native 1366x768 + pause menu (CIRCLE/ESC: resume/save/quit)
- [ ] C2. Async chunk remesh (edits already 4 ms; matters once the world grows)
- [ ] C3. Bigger world / chunk streaming from disk
- [ ] C4. Save versioning + player position saved with the world

**Phase D — dreams**
- [ ] D1. LAN multiplayer (the laptop joins the den as player 2)
- [ ] D2. Simple mobs (wandering animals; the Atom sets the population cap)

## Style

Plain C (C99), COBJMACROS-style D3D9 (no C++), same cross-compile pattern as
`~/projects/personal/xp-launcher` and the proven `tri_d3d9.c` in the maintenance
repo's `xp-gpu-trace/`.
