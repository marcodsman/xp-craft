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

**Phase A — feels like a game** (all landed overnight 2026-07-05)
- [x] A1. Gravity, jumping, block collision (walk mode; F / double-tap CROSS toggles fly)
- [x] A2. Hotbar UI: 8 block tiles, selection frame + name label; keys 1-8, SQUARE/L1/R1
      or mouse wheel cycle
- [x] A3. Day/night: 20-min loop, sky+fog lerp, world dimmed via TEXTUREFACTOR stage;
      clock in the stats line (tod 0 = 06:00 so the label matches the light)
- [x] A4. Sounds: dig/place/step/jump generated at startup, 4 waveOut voices (kernel
      mixer blends), `volume=N` arg, muted in bench. (Code-verified; audible check
      needs ears at the box.)

**Phase B — a world worth exploring** (verified by screenshot + world.dat forensics)
- [x] B1. Value-noise fBm terrain (continents + hills octaves)
- [x] B2. World is now 512x512x64 (32x32 chunk columns)
- [x] B3. ~2,900 trees (log trunks, ragged leaf canopies), sand beaches near water
- [x] B4. Translucent water lakes (second render pass, no z-write, alpha 160; raycast
      passes through; swim-ish physics: buoyant sink, CROSS paddles up)
- [x] B5. 3D-noise caves (8k+ sampled underground air cells), coal veins (190k blocks)
      and cobble patches in the stone

**Phase C — polish**
- [x] C1. Borderless fullscreen at desktop res (`windowed` arg for a window) + pause
      menu (ESC/START: resume / save / quit) + GDI-rendered font atlas for all text
- [x] C2. Meshing runs on a worker thread; edits re-queue at high priority
- [x] C3. Chunks stream in on demand (nearest-first), VBs evict beyond range+80;
      whole world stays in RAM (16 MB), save is one 16 MB blob
- [x] C4. Save v2: dims-validated header + player pos/look/mode/hotbar + time of day;
      autosave every 3 min; `world-v1.bak` kept from the old format

**Bench, new world (800x600 windowed):** r=32 → 43 FPS, r=48 → 33, r=64 → 25,
r=96 → 17, r=128 → 11. Default range 48. The forest costs ~3x the old flat world's
triangles — leaves defeat greedy meshing by design (ragged corners).

**Dev args:** `windowed`, `bench`, `time=0..1` (freeze time of day), `volume=0..100`,
`at=x,z` (teleport, flying).

**Phase E — the survival loop** (design copied from Minecraft/MineClone2 data,
running on our engine)
- [x] E1. Drops + inventory: breaking yields (stone→cobble, grass→dirt, rest drop
      themselves), placing costs; hotbar counts, gray empty slots. WALK = survival,
      FLY = creative (instant break, infinite blocks). Save v3 carries the inventory.
- [x] E2. Break-times by hardness (hold to dig), crack overlay, particle burst
- [x] E3. First recipe: C / TRIANGLE = 1 log → 4 planks
- [x] E4. Health, fall damage, drowning, death/respawn (hearts + bubbles HUD, red
      flash, hurt sound; creative invulnerable; inventory kept on death)
- [x] E5. Tools (wood/stone pickaxe/axe/shovel; AUTO-APPLY to block class — no
      hotbar juggling on a pad) + crafting menu (C/TRIANGLE, 8 MC recipes, pad nav).
      Real MC hardness restored: stone 7.5s by hand, 1.1s wood pick, 0.56s stone pick.
- [x] E6. Mobs: pigs by day (drop porkchops -> +4 hearts via the menu), zombies at
      night (chase, melee, burn at dawn), swords (wood 4 / stone 5 dmg, auto-apply),
      knockback, population caps sized for the Atom. Nothing survives the save file.

**Phase D — dreams**
- [ ] D1. LAN multiplayer (the laptop joins the den as player 2)
- [ ] D2. Simple mobs (wandering animals; the Atom sets the population cap)

## Style

Plain C (C99), COBJMACROS-style D3D9 (no C++), same cross-compile pattern as
`~/projects/personal/xp-launcher` and the proven `tri_d3d9.c` in the maintenance
repo's `xp-gpu-trace/`.
