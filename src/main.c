/* xp-craft — a from-scratch voxel game for the GMA500 XP box.
 *
 * Overnight build covering roadmap phases A2-A4, B1-B5, C1-C4:
 *   world:  512x512x64, value-noise terrain, caves, coal, trees, sand
 *           beaches, translucent water lakes (swimmable-ish)
 *   render: D3D9 fixed-function, greedy meshes built on a worker thread,
 *           chunks stream in on demand and evict when far, day/night via
 *           TEXTUREFACTOR light + sky/fog lerp, borderless fullscreen
 *   game:   walk/fly (F / double-tap CROSS), hotbar with block tiles,
 *           pause menu (ESC/START), waveOut sounds, autosave,
 *           save v2 carries player pos/look/mode + time of day
 *
 * Args: bench | windowed | time=0..1 (freeze time of day) | volume=0..100
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "pad.h"                    /* ../xp-pad/src — shared pad mapping */

/* ------------------------------------------------------------ constants -- */

#define CHUNK    16
#define WORLD_CX 32
#define WORLD_CZ 32
#define WORLD_H  64
#define WX (WORLD_CX * CHUNK)
#define WZ (WORLD_CZ * CHUNK)
#define WATER_Y  24                 /* lake surface level */

#define TILE 64
#define REACH 6.0f

#define EYE_H     1.62f
#define BOX_HALF  0.3f
#define BOX_TOP   0.18f
#define GRAVITY   28.0f
#define JUMP_V    8.2f
#define WALK_SPD  4.3f
#define FLY_SPD   10.0f

#define DAY_SECONDS 1200.0f         /* full day/night loop (20 min) */
#define AUTOSAVE_S  180.0f

enum { B_AIR, B_GRASS, B_DIRT, B_STONE, B_PLANKS, B_LOG, B_LEAVES,
       B_SAND, B_WATER, B_COBBLE, B_COAL, NBLOCKS };
enum { F_TOP, F_BOTTOM, F_NORTH, F_SOUTH, F_WEST, F_EAST };
enum { T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_PLANKS, T_LOG_SIDE,
       T_LOG_TOP, T_LEAVES, T_SAND, T_WATER, T_COBBLE, T_COAL, NTILES };

static const char *BLOCK_NAME[NBLOCKS] = {
    "air", "grass", "dirt", "stone", "planks", "log", "leaves",
    "sand", "water", "cobble", "coal",
};

/* survival design data, lifted from the Minecraft/MineClone2 tables.
 * hardness = seconds to break by hand (stone-class softened to 3s until
 * tools exist; MC's bare-hand 7.5s assumes you rush a wooden pickaxe). */
static const float HARDNESS[NBLOCKS] = {
    [B_GRASS] = 0.9f, [B_DIRT] = 0.75f, [B_STONE] = 3.0f,
    [B_PLANKS] = 2.0f, [B_LOG] = 2.0f, [B_LEAVES] = 0.3f,
    [B_SAND] = 0.75f, [B_COBBLE] = 3.0f, [B_COAL] = 3.0f,
};
static const BYTE DROPS[NBLOCKS] = {    /* what breaking yields */
    [B_GRASS] = B_DIRT, [B_DIRT] = B_DIRT, [B_STONE] = B_COBBLE,
    [B_PLANKS] = B_PLANKS, [B_LOG] = B_LOG, [B_LEAVES] = B_LEAVES,
    [B_SAND] = B_SAND, [B_COBBLE] = B_COBBLE, [B_COAL] = B_COAL,
};

/* hotbar */
static const BYTE HOTBAR[] = { B_GRASS, B_DIRT, B_STONE, B_PLANKS,
                               B_LOG, B_LEAVES, B_SAND, B_COBBLE };
#define NHOTBAR ((int)sizeof HOTBAR)

typedef struct { float x, y, z; DWORD color; float u, v; } VTX;
#define VTX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct { float x, y, z, rhw; DWORD color; float u, v; } HVTX;
#define HVTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

/* chunk lifecycle: phase gates the queue, vb/ib draw whenever present */
enum { PH_IDLE, PH_QUEUED, PH_MESHING, PH_READY };

typedef struct {
    volatile LONG phase;
    int dirty;                      /* world changed since last mesh */
    /* worker output, waiting for VB upload on the main thread */
    VTX  *pverts;
    WORD *pidx;
    int   pnv, pib_start[NTILES], pprims[NTILES];
    /* live GPU buffers */
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9  *ib;
    int ib_start[NTILES], prims[NTILES];
    int nverts, ntris;
    float min[3], max[3];
} CHUNKMESH;

/* ------------------------------------------------------------- globals --- */

static IDirect3D9        *g_d3d;
static IDirect3DDevice9  *g_dev;
static IDirect3DTexture9 *g_tex[NTILES];
static IDirect3DTexture9 *g_font;
static D3DPRESENT_PARAMETERS g_pp;
static CHUNKMESH g_chunks[WORLD_CZ][WORLD_CX];
static BYTE (*g_world)[WZ][WX];
static int  g_running = 1;
static int  g_hwvp;
static HWND g_hwnd;
static int  g_win_w = 800, g_win_h = 600;
static int  g_windowed;
static int  g_glyph_w = 10;

static float g_range = 48.0f;      /* ~30 FPS on the box (bench curve) */

/* camera = player eye */
static float g_cam_x, g_cam_y, g_cam_z;
static float g_yaw, g_pitch;
static int   g_manual;

static int   g_walk = 1;
static float g_vel_y;
static int   g_on_ground, g_in_water;
static float g_move_s, g_move_f, g_move_u;
static int   g_jump;
static float g_step_dist;

static int   g_hotbar_idx = 3;      /* start on planks */
static int   g_sel = B_PLANKS;

/* survival: inventory + block-breaking progress (fly mode = creative) */
static int   g_inv[NBLOCKS];
static int   g_break_on;            /* currently digging */
static int   g_break_held;          /* dig input held this frame */
static int   g_break_cell[3];
static float g_break_prog;          /* 0..1 */
static IDirect3DTexture9 *g_crack;

/* first-person hand: the held block swings while digging/placing */
static float g_swing;               /* 0..1 animation phase */
static int   g_swing_on;
static float g_bob;                 /* walk-bob phase */

/* health & hazards (survival only; MC numbers: 20 half-hearts,
 * fall damage = blocks - 3, ~15s of air then drowning) */
static int   g_health = 20;
static float g_air = 10.0f;
static int   g_dead;
static float g_fall_from = -1;      /* y where the current fall began */
static float g_hurt_t;              /* red flash timer */
static float g_regen_t, g_drown_t;
static float g_spawn_x, g_spawn_y, g_spawn_z;

/* particles */
#define NPART 96
static struct {
    float x, y, z, vx, vy, vz, life;
    int tile;
} g_part[NPART];
static int   g_remesh_ms;
static float g_click_cd;
static char  g_world_file[MAX_PATH];

static float g_tod = 0.30f;         /* time of day 0..1; 0.30 = morning */
static int   g_tod_frozen;
static float g_daylight = 1.0f;
static DWORD g_sky_color;
static float g_autosave_t;
static float g_toast_t;             /* "SAVED" toast timer */
static char  g_toast[32];

static int   g_paused, g_menu_sel;
static int   g_stats = 1;

static int   g_pad_seen, g_pad_mapped;

/* bench */
static int    g_bench;
static int    g_bench_idx, g_bench_warm;
static double g_bench_t0, g_bench_frames;
static const float BENCH_RANGES[] = { 32, 48, 64, 96, 128 };
#define NBENCH ((int)(sizeof BENCH_RANGES / sizeof BENCH_RANGES[0]))
static char   g_bench_log[1024];

/* ---------------------------------------------------------------- mat4 --- */
/* D3D row-vector convention: v' = v * M. */

static D3DMATRIX mat_identity(void)
{
    D3DMATRIX m = {{{0}}};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

static D3DMATRIX mat_mul(const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j]
                      + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    return r;
}

static D3DMATRIX mat_rot_x(float a)
{
    D3DMATRIX m = mat_identity();
    m.m[1][1] = cosf(a); m.m[1][2] = sinf(a);
    m.m[2][1] = -sinf(a); m.m[2][2] = cosf(a);
    return m;
}

static D3DMATRIX mat_rot_y(float a)
{
    D3DMATRIX m = mat_identity();
    m.m[0][0] = cosf(a); m.m[0][2] = -sinf(a);
    m.m[2][0] = sinf(a); m.m[2][2] = cosf(a);
    return m;
}

static D3DMATRIX mat_translate(float x, float y, float z)
{
    D3DMATRIX m = mat_identity();
    m.m[3][0] = x; m.m[3][1] = y; m.m[3][2] = z;
    return m;
}

static D3DMATRIX mat_perspective(float fovy, float aspect, float zn, float zf)
{
    float h = 1.0f / tanf(fovy * 0.5f);
    D3DMATRIX m = {{{0}}};
    m.m[0][0] = h / aspect;
    m.m[1][1] = h;
    m.m[2][2] = zf / (zf - zn);
    m.m[2][3] = 1.0f;
    m.m[3][2] = -zn * zf / (zf - zn);
    return m;
}

static D3DMATRIX mat_view(void)
{
    D3DMATRIX t  = mat_translate(-g_cam_x, -g_cam_y, -g_cam_z);
    D3DMATRIX ry = mat_rot_y(-g_yaw);
    D3DMATRIX rx = mat_rot_x(-g_pitch);
    D3DMATRIX m  = mat_mul(&t, &ry);
    return mat_mul(&m, &rx);
}

/* ------------------------------------------------------------- frustum --- */

static float g_planes[6][4];

static void frustum_from(const D3DMATRIX *vp)
{
    for (int i = 0; i < 4; i++) {
        g_planes[0][i] = vp->m[i][3] + vp->m[i][0];
        g_planes[1][i] = vp->m[i][3] - vp->m[i][0];
        g_planes[2][i] = vp->m[i][3] + vp->m[i][1];
        g_planes[3][i] = vp->m[i][3] - vp->m[i][1];
        g_planes[4][i] = vp->m[i][2];
        g_planes[5][i] = vp->m[i][3] - vp->m[i][2];
    }
}

static int aabb_visible(const float min[3], const float max[3])
{
    for (int p = 0; p < 6; p++) {
        const float *pl = g_planes[p];
        float x = pl[0] >= 0 ? max[0] : min[0];
        float y = pl[1] >= 0 ? max[1] : min[1];
        float z = pl[2] >= 0 ? max[2] : min[2];
        if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0)
            return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- noise --- */

static unsigned hash2u(int x, int z)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)z * 668265263u
               + 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static unsigned hash3u(int x, int y, int z)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 2246822519u
               + (unsigned)z * 668265263u + 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float grid2(int x, int z) { return (hash2u(x, z) & 0xffff) / 65535.0f; }
static float grid3(int x, int y, int z)
{
    return (hash3u(x, y, z) & 0xffff) / 65535.0f;
}

static float smooth(float t) { return t * t * (3 - 2 * t); }

static float vnoise2(float x, float z)
{
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = smooth(x - ix), fz = smooth(z - iz);
    float a = grid2(ix, iz),     b = grid2(ix + 1, iz);
    float c = grid2(ix, iz + 1), d = grid2(ix + 1, iz + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

static float vnoise3(float x, float y, float z)
{
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
    float v000 = grid3(ix, iy, iz),         v100 = grid3(ix + 1, iy, iz);
    float v010 = grid3(ix, iy + 1, iz),     v110 = grid3(ix + 1, iy + 1, iz);
    float v001 = grid3(ix, iy, iz + 1),     v101 = grid3(ix + 1, iy, iz + 1);
    float v011 = grid3(ix, iy + 1, iz + 1), v111 = grid3(ix + 1, iy + 1, iz + 1);
    float x00 = v000 + (v100 - v000) * fx, x10 = v010 + (v110 - v010) * fx;
    float x01 = v001 + (v101 - v001) * fx, x11 = v011 + (v111 - v011) * fx;
    float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}

static float fbm2(float x, float z)
{
    return vnoise2(x, z) * 0.5f + vnoise2(x * 2.03f, z * 2.03f) * 0.25f
         + vnoise2(x * 4.01f, z * 4.01f) * 0.125f
         + vnoise2(x * 8.07f, z * 8.07f) * 0.125f;
}

/* --------------------------------------------------------------- world ---- */

static int solid_block(int b) { return b != B_AIR && b != B_WATER; }

static int block_at(int x, int y, int z)
{
    if (y < 0) return B_STONE;
    if (x < 0 || z < 0 || x >= WX || z >= WZ || y >= WORLD_H) return B_AIR;
    return g_world[y][z][x];
}

static int terrain_h(int x, int z)
{
    float base = fbm2(x * 0.011f, z * 0.011f);          /* continents */
    float hill = fbm2(x * 0.045f + 91.7f, z * 0.045f + 33.3f);
    float h = 16.0f + base * 34.0f + (hill - 0.5f) * 10.0f;
    int hi = (int)h;
    if (hi < 3) hi = 3;
    if (hi > WORLD_H - 10) hi = WORLD_H - 10;
    return hi;
}

static void gen_world(void)
{
    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) {
            int h = terrain_h(x, z);
            int sandy = h <= WATER_Y + 2;
            for (int y = 0; y < h; y++) {
                BYTE b;
                if (y >= h - 1)      b = sandy ? B_SAND : B_GRASS;
                else if (y >= h - 4) b = sandy ? B_SAND : B_DIRT;
                else {
                    b = B_STONE;
                    if (grid3(x, y, z) > 0.972f) b = B_COAL;   /* veins */
                    else if (grid3(x + 917, y, z + 313) > 0.985f) b = B_COBBLE;
                }
                g_world[y][z][x] = b;
            }
            /* lakes fill open air up to WATER_Y */
            if (h <= WATER_Y)
                for (int y = h; y <= WATER_Y; y++)
                    g_world[y][z][x] = B_WATER;
        }

    /* caves: carve 3D noise tunnels through stone (kept above y=1) */
    for (int y = 2; y < WORLD_H - 12; y++)
        for (int z = 0; z < WZ; z++)
            for (int x = 0; x < WX; x++) {
                if (g_world[y][z][x] == B_AIR || g_world[y][z][x] == B_WATER)
                    continue;
                float n = vnoise3(x * 0.075f, y * 0.11f, z * 0.075f);
                if (n > 0.73f && g_world[y + 1][z][x] != B_WATER)
                    g_world[y][z][x] = B_AIR;
            }

    /* trees on grass, clear of water */
    for (int z = 3; z < WZ - 3; z++)
        for (int x = 3; x < WX - 3; x++) {
            if ((hash2u(x * 3 + 7, z * 3 + 11) & 1023) >= 14) continue;
            int h = terrain_h(x, z);
            if (h + 8 >= WORLD_H) continue;
            if (g_world[h - 1][z][x] != B_GRASS) continue;
            int th = 4 + (int)(hash2u(x, z) % 3);
            for (int y = h; y < h + th; y++)
                g_world[y][z][x] = B_LOG;
            for (int ly = h + th - 2; ly <= h + th + 1; ly++) {
                int r = ly >= h + th ? 1 : 2;
                for (int dz = -r; dz <= r; dz++)
                    for (int dx = -r; dx <= r; dx++) {
                        if (dx == 0 && dz == 0 && ly < h + th) continue;
                        if (abs(dx) == r && abs(dz) == r &&
                            (hash2u(x + dx * 7, z + dz * 13 + ly) & 1))
                            continue;   /* ragged corners */
                        if (g_world[ly][z + dz][x + dx] == B_AIR)
                            g_world[ly][z + dz][x + dx] = B_LEAVES;
                    }
            }
        }
}

/* ------------------------------------------------------------- meshing --- */
/* Same proven corner/winding scheme as before. Runs on the WORKER thread. */

static const struct {
    int dx, dy, dz;
    BYTE c[4][3];
    int shade;
} FACES[6] = {
    [F_TOP]    = { 0, 1, 0, {{0,1,0},{1,1,0},{1,1,1},{0,1,1}}, 100 },
    [F_BOTTOM] = { 0,-1, 0, {{0,0,1},{1,0,1},{1,0,0},{0,0,0}},  50 },
    [F_NORTH]  = { 0, 0,-1, {{1,1,0},{0,1,0},{0,0,0},{1,0,0}},  80 },
    [F_SOUTH]  = { 0, 0, 1, {{0,1,1},{1,1,1},{1,0,1},{0,0,1}},  80 },
    [F_WEST]   = {-1, 0, 0, {{0,1,0},{0,1,1},{0,0,1},{0,0,0}},  60 },
    [F_EAST]   = { 1, 0, 0, {{1,1,1},{1,1,0},{1,0,0},{1,0,1}},  60 },
};

static const BYTE TILE_FOR[NBLOCKS][6] = {
    [B_GRASS]  = { T_GRASS_TOP, T_DIRT, T_GRASS_SIDE, T_GRASS_SIDE,
                   T_GRASS_SIDE, T_GRASS_SIDE },
    [B_DIRT]   = { T_DIRT, T_DIRT, T_DIRT, T_DIRT, T_DIRT, T_DIRT },
    [B_STONE]  = { T_STONE, T_STONE, T_STONE, T_STONE, T_STONE, T_STONE },
    [B_PLANKS] = { T_PLANKS, T_PLANKS, T_PLANKS, T_PLANKS, T_PLANKS,
                   T_PLANKS },
    [B_LOG]    = { T_LOG_TOP, T_LOG_TOP, T_LOG_SIDE, T_LOG_SIDE,
                   T_LOG_SIDE, T_LOG_SIDE },
    [B_LEAVES] = { T_LEAVES, T_LEAVES, T_LEAVES, T_LEAVES, T_LEAVES,
                   T_LEAVES },
    [B_SAND]   = { T_SAND, T_SAND, T_SAND, T_SAND, T_SAND, T_SAND },
    [B_WATER]  = { T_WATER, T_WATER, T_WATER, T_WATER, T_WATER, T_WATER },
    [B_COBBLE] = { T_COBBLE, T_COBBLE, T_COBBLE, T_COBBLE, T_COBBLE,
                   T_COBBLE },
    [B_COAL]   = { T_COAL, T_COAL, T_COAL, T_COAL, T_COAL, T_COAL },
};

typedef struct { BYTE tile, face; BYTE x0, x1, y0, y1, z0, z1; } QUAD;
#define MAX_QUADS 16000             /* 4*16000 = 64000 verts < 65536 */
static QUAD g_quads[MAX_QUADS];     /* worker-thread only */
static int  g_nquads;

static void greedy_2d(BYTE *mask, int nu, int nv,
                      void (*emit)(int, int, int, int, int, void *), void *ctx)
{
    for (int v = 0; v < nv; v++)
        for (int u = 0; u < nu; u++) {
            int t = mask[v * nu + u];
            if (!t) continue;
            int w = 1, h = 1;
            while (u + w < nu && mask[v * nu + u + w] == t) w++;
            for (; v + h < nv; h++) {
                int ok = 1;
                for (int i = 0; i < w; i++)
                    if (mask[(v + h) * nu + u + i] != t) { ok = 0; break; }
                if (!ok) break;
            }
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    mask[(v + j) * nu + u + i] = 0;
            emit(u, v, w, h, t, ctx);
        }
}

typedef struct { int face, slice; } EMITCTX;

static void emit_rect(int u0, int v0, int w, int h, int tile, void *vctx)
{
    if (g_nquads >= MAX_QUADS) return;
    EMITCTX *e = vctx;
    QUAD *q = &g_quads[g_nquads++];
    q->tile = (BYTE)tile;
    q->face = (BYTE)e->face;
    switch (e->face) {
    case F_TOP: case F_BOTTOM:
        q->x0 = u0; q->x1 = u0 + w - 1;
        q->z0 = v0; q->z1 = v0 + h - 1;
        q->y0 = q->y1 = e->slice;
        break;
    case F_NORTH: case F_SOUTH:
        q->x0 = u0; q->x1 = u0 + w - 1;
        q->y0 = v0; q->y1 = v0 + h - 1;
        q->z0 = q->z1 = e->slice;
        break;
    default:
        q->z0 = u0; q->z1 = u0 + w - 1;
        q->y0 = v0; q->y1 = v0 + h - 1;
        q->x0 = q->x1 = e->slice;
    }
}

static void quad_uv_extent(const QUAD *q, int *w, int *h)
{
    switch (q->face) {
    case F_TOP: case F_BOTTOM:
        *w = q->x1 - q->x0 + 1; *h = q->z1 - q->z0 + 1; break;
    case F_NORTH: case F_SOUTH:
        *w = q->x1 - q->x0 + 1; *h = q->y1 - q->y0 + 1; break;
    default:
        *w = q->z1 - q->z0 + 1; *h = q->y1 - q->y0 + 1; break;
    }
}

/* face of `blk` at world (x,y,z) toward neighbor `nb`? */
static int face_visible(int blk, int nb)
{
    if (blk == B_WATER) return nb == B_AIR;
    return !solid_block(nb);        /* solids show against air AND water */
}

static void mesh_chunk_quads(int cx, int cz)
{
    static BYTE mask[CHUNK * WORLD_H];  /* worker-thread only */
    int bx = cx * CHUNK, bz = cz * CHUNK;
    g_nquads = 0;
    EMITCTX e;

    for (int f = F_TOP; f <= F_BOTTOM; f++)
        for (int y = 0; y < WORLD_H; y++) {
            int any = 0;
            for (int z = 0; z < CHUNK; z++)
                for (int x = 0; x < CHUNK; x++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        face_visible(blk, block_at(bx + x, y + FACES[f].dy,
                                                   bz + z)))
                        t = TILE_FOR[blk][f] + 1;
                    mask[z * CHUNK + x] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = y;
            greedy_2d(mask, CHUNK, CHUNK, emit_rect, &e);
        }

    for (int f = F_NORTH; f <= F_SOUTH; f++)
        for (int z = 0; z < CHUNK; z++) {
            int any = 0;
            for (int y = 0; y < WORLD_H; y++)
                for (int x = 0; x < CHUNK; x++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        face_visible(blk, block_at(bx + x, y,
                                                   bz + z + FACES[f].dz)))
                        t = TILE_FOR[blk][f] + 1;
                    mask[y * CHUNK + x] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = z;
            greedy_2d(mask, CHUNK, WORLD_H, emit_rect, &e);
        }

    for (int f = F_WEST; f <= F_EAST; f++)
        for (int x = 0; x < CHUNK; x++) {
            int any = 0;
            for (int y = 0; y < WORLD_H; y++)
                for (int z = 0; z < CHUNK; z++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        face_visible(blk, block_at(bx + x + FACES[f].dx, y,
                                                   bz + z)))
                        t = TILE_FOR[blk][f] + 1;
                    mask[y * CHUNK + z] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = x;
            greedy_2d(mask, CHUNK, WORLD_H, emit_rect, &e);
        }
}

/* worker: mesh one chunk into malloc'd vertex/index arrays */
static void worker_build(int cx, int cz)
{
    CHUNKMESH *c = &g_chunks[cz][cx];
    int bx = cx * CHUNK, bz = cz * CHUNK;

    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);

    mesh_chunk_quads(cx, cz);

    int nverts = g_nquads * 4, nidx = g_nquads * 6;
    VTX  *verts = NULL;
    WORD *idx = NULL;
    int vi = 0, ii = 0;
    if (g_nquads) {
        verts = malloc(sizeof(VTX) * nverts);
        idx   = malloc(sizeof(WORD) * nidx);
    }
    for (int t = 0; t < NTILES && verts; t++) {
        c->pib_start[t] = ii;
        for (int n = 0; n < g_nquads; n++) {
            const QUAD *q = &g_quads[n];
            if (q->tile != t + 1) continue;
            int s = 255 * FACES[q->face].shade / 100;
            DWORD col = (t == T_WATER) ? D3DCOLOR_ARGB(160, s, s, s)
                                       : D3DCOLOR_ARGB(255, s, s, s);
            int uw, vh;
            quad_uv_extent(q, &uw, &vh);
            float uvw[4][2] = { {0,0}, {(float)uw,0},
                                {(float)uw,(float)vh}, {0,(float)vh} };
            for (int k = 0; k < 4; k++) {
                const BYTE *o = FACES[q->face].c[k];
                verts[vi + k].x = (float)(bx + (o[0] ? q->x1 + 1 : q->x0));
                verts[vi + k].y = (float)(     (o[1] ? q->y1 + 1 : q->y0));
                verts[vi + k].z = (float)(bz + (o[2] ? q->z1 + 1 : q->z0));
                verts[vi + k].color = col;
                verts[vi + k].u = uvw[k][0];
                verts[vi + k].v = uvw[k][1];
            }
            idx[ii++] = (WORD)vi;       idx[ii++] = (WORD)(vi + 1);
            idx[ii++] = (WORD)(vi + 2); idx[ii++] = (WORD)vi;
            idx[ii++] = (WORD)(vi + 2); idx[ii++] = (WORD)(vi + 3);
            vi += 4;
        }
        c->pprims[t] = (ii - c->pib_start[t]) / 3;
    }
    c->pverts = verts;
    c->pidx = idx;
    c->pnv = vi;

    c->min[0] = (float)bx; c->min[1] = 0; c->min[2] = (float)bz;
    c->max[0] = (float)(bx + CHUNK);
    c->max[1] = (float)WORLD_H;
    c->max[2] = (float)(bz + CHUNK);

    QueryPerformanceCounter(&b);
    g_remesh_ms = (int)((b.QuadPart - a.QuadPart) * 1000 / freq.QuadPart);
}

/* --------------------------------------------------- mesh queue + thread -- */

static CRITICAL_SECTION g_qlock;
static int  g_qhi[256], g_qlo[WORLD_CX * WORLD_CZ];
static int  g_nhi, g_nlo;
static HANDLE g_worker;
static HANDLE g_wake;
static volatile LONG g_worker_run = 1;

static void queue_push(int cx, int cz, int hi)
{
    CHUNKMESH *c = &g_chunks[cz][cx];
    if (InterlockedCompareExchange(&c->phase, PH_QUEUED, PH_IDLE) != PH_IDLE)
        return;
    EnterCriticalSection(&g_qlock);
    int key = cz * WORLD_CX + cx;
    if (hi) { if (g_nhi < 256) g_qhi[g_nhi++] = key; }
    else    { if (g_nlo < WORLD_CX * WORLD_CZ) g_qlo[g_nlo++] = key; }
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_wake);
}

static int queue_pop(void)
{
    int key = -1;
    EnterCriticalSection(&g_qlock);
    if (g_nhi) key = g_qhi[--g_nhi];
    else if (g_nlo) {
        /* pop the chunk closest to the player for nice fill-in order */
        int best = 0;
        float bd = 1e30f;
        for (int i = 0; i < g_nlo; i++) {
            int cx = g_qlo[i] % WORLD_CX, cz = g_qlo[i] / WORLD_CX;
            float dx = cx * CHUNK + 8 - g_cam_x;
            float dz = cz * CHUNK + 8 - g_cam_z;
            float d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = i; }
        }
        key = g_qlo[best];
        g_qlo[best] = g_qlo[--g_nlo];
    }
    LeaveCriticalSection(&g_qlock);
    return key;
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    (void)arg;
    while (g_worker_run) {
        int key = queue_pop();
        if (key < 0) {
            WaitForSingleObject(g_wake, 100);
            continue;
        }
        int cx = key % WORLD_CX, cz = key / WORLD_CX;
        CHUNKMESH *c = &g_chunks[cz][cx];
        c->phase = PH_MESHING;
        c->dirty = 0;
        worker_build(cx, cz);
        c->phase = PH_READY;
    }
    return 0;
}

static int queue_busy(void)
{
    if (g_nhi || g_nlo) return 1;
    for (int cz = 0; cz < WORLD_CZ; cz++)
        for (int cx = 0; cx < WORLD_CX; cx++)
            if (g_chunks[cz][cx].phase != PH_IDLE) return 1;
    return 0;
}

/* main thread: upload finished meshes (budgeted), stream + evict */
static void stream_chunks(void)
{
    int uploads = g_bench ? 16 : 5;
    for (int cz = 0; cz < WORLD_CZ && uploads; cz++)
        for (int cx = 0; cx < WORLD_CX && uploads; cx++) {
            CHUNKMESH *c = &g_chunks[cz][cx];
            if (c->phase != PH_READY) continue;
            if (c->vb) { IDirect3DVertexBuffer9_Release(c->vb); c->vb = NULL; }
            if (c->ib) { IDirect3DIndexBuffer9_Release(c->ib); c->ib = NULL; }
            if (c->pnv) {
                UINT vbytes = c->pnv * sizeof(VTX);
                int nidx = 0;
                for (int t = 0; t < NTILES; t++)
                    nidx = c->pib_start[t] + c->pprims[t] * 3;
                UINT ibytes = nidx * sizeof(WORD);
                void *p;
                if (SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(g_dev,
                        vbytes, D3DUSAGE_WRITEONLY, VTX_FVF,
                        D3DPOOL_MANAGED, &c->vb, NULL)) &&
                    SUCCEEDED(IDirect3DDevice9_CreateIndexBuffer(g_dev,
                        ibytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                        D3DPOOL_MANAGED, &c->ib, NULL))) {
                    IDirect3DVertexBuffer9_Lock(c->vb, 0, vbytes, &p, 0);
                    memcpy(p, c->pverts, vbytes);
                    IDirect3DVertexBuffer9_Unlock(c->vb);
                    IDirect3DIndexBuffer9_Lock(c->ib, 0, ibytes, &p, 0);
                    memcpy(p, c->pidx, ibytes);
                    IDirect3DIndexBuffer9_Unlock(c->ib);
                    memcpy(c->ib_start, c->pib_start, sizeof c->ib_start);
                    memcpy(c->prims, c->pprims, sizeof c->prims);
                    c->nverts = c->pnv;
                    c->ntris = 0;
                    for (int t = 0; t < NTILES; t++) c->ntris += c->prims[t];
                } else {
                    if (c->vb) { IDirect3DVertexBuffer9_Release(c->vb); c->vb = NULL; }
                    c->ib = NULL;
                }
            } else {
                c->nverts = c->ntris = 0;
                memset(c->prims, 0, sizeof c->prims);
            }
            free(c->pverts); c->pverts = NULL;
            free(c->pidx);   c->pidx = NULL;
            c->phase = PH_IDLE;
            uploads--;
        }

    /* enqueue missing/dirty chunks in range; evict far ones */
    float build_r = g_range + CHUNK * 1.5f;
    float evict_r = g_range + CHUNK * 5.0f;
    for (int cz = 0; cz < WORLD_CZ; cz++)
        for (int cx = 0; cx < WORLD_CX; cx++) {
            CHUNKMESH *c = &g_chunks[cz][cx];
            float dx = cx * CHUNK + 8 - g_cam_x;
            float dz = cz * CHUNK + 8 - g_cam_z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d <= build_r && c->phase == PH_IDLE &&
                (!c->vb ? 1 : c->dirty))
                queue_push(cx, cz, c->dirty);
            else if (d > evict_r && c->phase == PH_IDLE && c->vb) {
                IDirect3DVertexBuffer9_Release(c->vb); c->vb = NULL;
                if (c->ib) { IDirect3DIndexBuffer9_Release(c->ib); c->ib = NULL; }
                c->ntris = 0;
            }
        }
}

/* ------------------------------------------------------------- editing --- */

static void mark_dirty(int cx, int cz)
{
    if (cx < 0 || cz < 0 || cx >= WORLD_CX || cz >= WORLD_CZ) return;
    g_chunks[cz][cx].dirty = 1;
    queue_push(cx, cz, 1);
}

static void play_sound(int which);     /* fwd */

static void set_block(int x, int y, int z, int b)
{
    if (x < 0 || y < 1 || z < 0 || x >= WX || y >= WORLD_H || z >= WZ)
        return;
    g_world[y][z][x] = (BYTE)b;
    int cx = x / CHUNK, cz = z / CHUNK;
    mark_dirty(cx, cz);
    if (x % CHUNK == 0)         mark_dirty(cx - 1, cz);
    if (x % CHUNK == CHUNK - 1) mark_dirty(cx + 1, cz);
    if (z % CHUNK == 0)         mark_dirty(cx, cz - 1);
    if (z % CHUNK == CHUNK - 1) mark_dirty(cx, cz + 1);
}

static void cam_dir(float *dx, float *dy, float *dz)
{
    *dx = sinf(g_yaw) * cosf(g_pitch);
    *dy = -sinf(g_pitch);
    *dz = cosf(g_yaw) * cosf(g_pitch);
}

/* Amanatides-Woo; water is transparent to the ray */
static int raycast(int hit[3], int prev[3])
{
    float dx, dy, dz;
    cam_dir(&dx, &dy, &dz);

    int   cx = (int)floorf(g_cam_x), cy = (int)floorf(g_cam_y),
          cz = (int)floorf(g_cam_z);
    int   sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1, sz = dz >= 0 ? 1 : -1;
    float tdx = dx != 0 ? fabsf(1.0f / dx) : 1e30f;
    float tdy = dy != 0 ? fabsf(1.0f / dy) : 1e30f;
    float tdz = dz != 0 ? fabsf(1.0f / dz) : 1e30f;
    float tmx = dx != 0 ? (dx > 0 ? (cx + 1 - g_cam_x) : (g_cam_x - cx)) * tdx : 1e30f;
    float tmy = dy != 0 ? (dy > 0 ? (cy + 1 - g_cam_y) : (g_cam_y - cy)) * tdy : 1e30f;
    float tmz = dz != 0 ? (dz > 0 ? (cz + 1 - g_cam_z) : (g_cam_z - cz)) * tdz : 1e30f;

    prev[0] = cx; prev[1] = cy; prev[2] = cz;
    float t = 0;
    while (t <= REACH) {
        prev[0] = cx; prev[1] = cy; prev[2] = cz;
        if      (tmx <= tmy && tmx <= tmz) { t = tmx; tmx += tdx; cx += sx; }
        else if (tmy <= tmz)               { t = tmy; tmy += tdy; cy += sy; }
        else                               { t = tmz; tmz += tdz; cz += sz; }
        if (t > REACH) break;
        int b = block_at(cx, cy, cz);
        if (b != B_AIR && b != B_WATER) {
            hit[0] = cx; hit[1] = cy; hit[2] = cz;
            return 1;
        }
    }
    return 0;
}

static void spawn_particles(int x, int y, int z, int blk, int n)
{
    int tile = TILE_FOR[blk][F_NORTH];
    for (int i = 0; i < NPART && n; i++) {
        if (g_part[i].life > 0) continue;
        unsigned h = hash3u(x * 31 + i, y * 17 + n, z * 13);
        g_part[i].x = x + 0.2f + (h & 63) / 100.0f;
        g_part[i].y = y + 0.2f + ((h >> 6) & 63) / 100.0f;
        g_part[i].z = z + 0.2f + ((h >> 12) & 63) / 100.0f;
        g_part[i].vx = (((h >> 18) & 15) - 7.5f) * 0.35f;
        g_part[i].vz = (((h >> 22) & 15) - 7.5f) * 0.35f;
        g_part[i].vy = 2.0f + ((h >> 26) & 7) * 0.4f;
        g_part[i].life = 0.5f + ((h >> 29) & 3) * 0.1f;
        g_part[i].tile = tile;
        n--;
    }
}

static void update_particles(float dt)
{
    for (int i = 0; i < NPART; i++) {
        if (g_part[i].life <= 0) continue;
        g_part[i].life -= dt;
        g_part[i].vy -= 18.0f * dt;
        g_part[i].x += g_part[i].vx * dt;
        g_part[i].y += g_part[i].vy * dt;
        g_part[i].z += g_part[i].vz * dt;
    }
}

static void swing_kick(void)
{
    if (!g_swing_on) {
        g_swing_on = 1;
        g_swing = 0;
    }
}

static void damage(int hp)
{
    if (!g_walk || g_dead) return;      /* creative is invulnerable */
    g_health -= hp;
    g_hurt_t = 0.35f;
    g_regen_t = 0;
    play_sound(4);
    if (g_health <= 0) {
        g_health = 0;
        g_dead = 1;
        g_break_on = 0;
    }
}

static void respawn(void)
{
    g_dead = 0;
    g_health = 20;
    g_air = 10.0f;
    g_vel_y = 0;
    g_fall_from = -1;
    g_cam_x = g_spawn_x;
    g_cam_y = g_spawn_y;
    g_cam_z = g_spawn_z;
}

static void break_complete(int x, int y, int z)
{
    int blk = block_at(x, y, z);
    if (blk == B_AIR || blk == B_WATER) return;
    set_block(x, y, z, B_AIR);
    if (g_walk) {                       /* survival: the block drops */
        g_inv[DROPS[blk]]++;
        spawn_particles(x, y, z, blk, 10);
    } else
        spawn_particles(x, y, z, blk, 6);
    play_sound(0);
}

/* creative (fly): instant break on click/hold */
static void edit_break(void)
{
    int hit[3], prev[3];
    if (g_walk) return;                 /* survival digs via update_breaking */
    if (raycast(hit, prev) && hit[1] > 0 && hit[1] < WORLD_H) {
        break_complete(hit[0], hit[1], hit[2]);
        swing_kick();
    }
}

/* survival: hold-to-dig with per-block hardness */
static void update_breaking(float dt, int held)
{
    int hit[3], prev[3];
    if (!held || !g_walk || g_paused || !raycast(hit, prev) ||
        hit[1] <= 0 || hit[1] >= WORLD_H) {
        g_break_on = 0;
        g_break_prog = 0;
        return;
    }
    if (!g_break_on || hit[0] != g_break_cell[0] ||
        hit[1] != g_break_cell[1] || hit[2] != g_break_cell[2]) {
        g_break_on = 1;
        g_break_cell[0] = hit[0];
        g_break_cell[1] = hit[1];
        g_break_cell[2] = hit[2];
        g_break_prog = 0;
    }
    float hard = HARDNESS[block_at(hit[0], hit[1], hit[2])];
    if (hard <= 0) hard = 1;
    g_break_prog += dt / hard;
    if (g_break_prog >= 1) {
        break_complete(hit[0], hit[1], hit[2]);
        g_break_on = 0;
        g_break_prog = 0;
    }
}

static void edit_place(void)
{
    int hit[3], prev[3];
    if (!raycast(hit, prev)) return;
    int pb = block_at(prev[0], prev[1], prev[2]);
    if (pb != B_AIR && pb != B_WATER) return;
    int ex = (int)floorf(g_cam_x), ey = (int)floorf(g_cam_y),
        ez = (int)floorf(g_cam_z);
    if (prev[0] == ex && prev[2] == ez &&
        (prev[1] == ey || prev[1] == ey - 1))
        return;
    if (g_walk) {                       /* survival: costs inventory */
        if (g_inv[g_sel] <= 0) return;
        g_inv[g_sel]--;
    }
    set_block(prev[0], prev[1], prev[2], g_sel);
    play_sound(1);
    swing_kick();
}

/* the one recipe so far (MineClone2 data: 1 log -> 4 planks) */
static void craft_planks(void)
{
    if (g_inv[B_LOG] >= 1) {
        g_inv[B_LOG]--;
        g_inv[B_PLANKS] += 4;
        strcpy(g_toast, "+4 PLANKS (1 LOG)");
        g_toast_t = 1.5f;
        play_sound(1);
    } else {
        strcpy(g_toast, "NEED A LOG TO CRAFT PLANKS");
        g_toast_t = 1.5f;
    }
}

/* ------------------------------------------------------ player physics --- */

static int box_hits(float ex, float ey, float ez)
{
    int x0 = (int)floorf(ex - BOX_HALF), x1 = (int)floorf(ex + BOX_HALF);
    int y0 = (int)floorf(ey - EYE_H),    y1 = (int)floorf(ey + BOX_TOP);
    int z0 = (int)floorf(ez - BOX_HALF), z1 = (int)floorf(ez + BOX_HALF);
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (solid_block(block_at(x, y, z)))
                    return 1;
    return 0;
}

static int move_axis(float d, int axis)
{
    if (d == 0) return 0;
    float *p = axis == 0 ? &g_cam_x : axis == 1 ? &g_cam_y : &g_cam_z;
    *p += d;
    if (!box_hits(g_cam_x, g_cam_y, g_cam_z)) return 0;

    if (axis == 1) {
        if (d < 0) {
            float feet = g_cam_y - EYE_H;
            g_cam_y = (floorf(feet) + 1.0f) + EYE_H + 0.001f;
        } else {
            float top = g_cam_y + BOX_TOP;
            g_cam_y = floorf(top) - BOX_TOP - 0.001f;
        }
    } else {
        float half = BOX_HALF;
        if (d > 0) *p = floorf(*p + half) - half - 0.001f;
        else       *p = (floorf(*p - half) + 1.0f) + half + 0.001f;
    }
    return 1;
}

static void step_player(float dt)
{
    float fx = sinf(g_yaw), fz = cosf(g_yaw);
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);
    float s = g_move_s, f = g_move_f;
    float mag = sqrtf(s * s + f * f);
    if (mag > 1) { s /= mag; f /= mag; }

    if (!g_walk) {
        float sp = FLY_SPD * dt;
        g_cam_x += (fx * f + rx * s) * sp;
        g_cam_z += (fz * f + rz * s) * sp;
        g_cam_y += g_move_u * sp;
        g_vel_y = 0;
        g_fall_from = -1;
        return;
    }

    g_in_water = block_at((int)floorf(g_cam_x),
                          (int)floorf(g_cam_y - EYE_H + 0.4f),
                          (int)floorf(g_cam_z)) == B_WATER;

    float spd = WALK_SPD * (g_in_water ? 0.6f : 1.0f);
    float mdx = (fx * f + rx * s) * spd * dt;
    float mdz = (fz * f + rz * s) * spd * dt;
    move_axis(mdx, 0);
    move_axis(mdz, 2);

    if (g_in_water) {
        g_vel_y -= 7.0f * dt;
        if (g_jump) g_vel_y = 2.8f;         /* paddle up */
        if (g_vel_y < -3.0f) g_vel_y = -3.0f;
        g_fall_from = -1;                   /* water breaks falls */
    } else {
        g_vel_y -= GRAVITY * dt;
        if (g_vel_y < -50) g_vel_y = -50;
        if (g_vel_y < 0 && g_fall_from < 0)
            g_fall_from = g_cam_y;          /* top of this fall */
    }

    int was_falling = g_vel_y < 0;
    g_on_ground = 0;
    if (move_axis(g_vel_y * dt, 1)) {
        if (was_falling) {
            g_on_ground = 1;
            if (g_fall_from >= 0) {         /* MC: blocks fallen - 3 */
                int dmg = (int)((g_fall_from - g_cam_y) - 3.0f);
                if (dmg > 0) damage(dmg);
                g_fall_from = -1;
            }
        }
        g_vel_y = 0;
    }
    if (g_jump && g_on_ground && !g_in_water) {
        g_vel_y = JUMP_V;
        g_on_ground = 0;
        play_sound(3);
    }

    /* footsteps + walk bob */
    if (g_on_ground && !g_in_water) {
        float moved = sqrtf(mdx * mdx + mdz * mdz);
        g_step_dist += moved;
        g_bob += moved * 3.2f;
        if (g_step_dist > 2.1f) {
            g_step_dist = 0;
            play_sound(2);
        }
    }
}

/* drowning + slow regen, per frame (survival only) */
static void update_survival(float dt)
{
    if (!g_walk || g_dead) {
        g_air = 10.0f;
        return;
    }
    int head_in = block_at((int)floorf(g_cam_x), (int)floorf(g_cam_y),
                           (int)floorf(g_cam_z)) == B_WATER;
    if (head_in) {
        g_air -= dt * (10.0f / 15.0f);      /* ~15s of breath */
        if (g_air <= 0) {
            g_air = 0;
            g_drown_t += dt;
            if (g_drown_t >= 1.0f) {
                g_drown_t = 0;
                damage(2);
            }
        }
    } else {
        g_air += dt * 4.0f;
        if (g_air > 10) g_air = 10;
        g_drown_t = 0;
    }
    if (g_health < 20) {                    /* peaceful-style regen */
        g_regen_t += dt;
        if (g_regen_t >= 4.0f) {
            g_regen_t = 0;
            g_health++;
        }
    }
}

/* --------------------------------------------------------- save / load --- */

static void world_file_path(void)
{
    GetModuleFileNameA(NULL, g_world_file, MAX_PATH);
    char *s = strrchr(g_world_file, '\\');
    if (s) strcpy(s + 1, "world.dat");
    else   strcpy(g_world_file, "world.dat");
}

static void save_world(void)
{
    FILE *fp = fopen(g_world_file, "wb");
    if (!fp) return;
    int hdr[4] = { 0x34435058 /* "XPC4" */, WX, WZ, WORLD_H };
    float st[8] = { g_cam_x, g_cam_y, g_cam_z, g_yaw, g_pitch,
                    (float)g_walk, (float)g_hotbar_idx, g_tod };
    float ext[5] = { g_spawn_x, g_spawn_y, g_spawn_z,
                     (float)g_health, g_air };
    fwrite(hdr, sizeof hdr, 1, fp);
    fwrite(st, sizeof st, 1, fp);
    fwrite(ext, sizeof ext, 1, fp);
    fwrite(g_inv, sizeof g_inv, 1, fp);
    fwrite(g_world, (size_t)WORLD_H * WZ * WX, 1, fp);
    fclose(fp);
    strcpy(g_toast, "WORLD SAVED");
    g_toast_t = 2.0f;
}

static int load_world(void)
{
    FILE *fp = fopen(g_world_file, "rb");
    if (!fp) return 0;
    int hdr[4] = {0};
    float st[8];
    if (fread(hdr, sizeof hdr, 1, fp) != 1 ||
        (hdr[0] != 0x34435058 && hdr[0] != 0x33435058 &&
         hdr[0] != 0x32435058) ||
        hdr[1] != WX || hdr[2] != WZ || hdr[3] != WORLD_H ||
        fread(st, sizeof st, 1, fp) != 1) {
        fclose(fp);
        return 0;
    }
    memset(g_inv, 0, sizeof g_inv);
    g_spawn_x = st[0]; g_spawn_y = st[1]; g_spawn_z = st[2];
    if (hdr[0] == 0x34435058) {         /* v4: spawn + vitals */
        float ext[5];
        if (fread(ext, sizeof ext, 1, fp) != 1 ||
            fread(g_inv, sizeof g_inv, 1, fp) != 1) {
            fclose(fp);
            return 0;
        }
        g_spawn_x = ext[0]; g_spawn_y = ext[1]; g_spawn_z = ext[2];
        g_health = (int)ext[3];
        g_air = ext[4];
        if (g_health <= 0 || g_health > 20) g_health = 20;
    } else if (hdr[0] == 0x33435058) {  /* v3: inventory only */
        if (fread(g_inv, sizeof g_inv, 1, fp) != 1) {
            fclose(fp);
            return 0;
        }
    }
    size_t ok = fread(g_world, (size_t)WORLD_H * WZ * WX, 1, fp);
    fclose(fp);
    if (ok != 1) return 0;
    g_cam_x = st[0]; g_cam_y = st[1]; g_cam_z = st[2];
    g_yaw = st[3]; g_pitch = st[4];
    g_walk = (int)st[5];
    g_hotbar_idx = (int)st[6];
    if (g_hotbar_idx < 0 || g_hotbar_idx >= NHOTBAR) g_hotbar_idx = 0;
    g_sel = HOTBAR[g_hotbar_idx];
    g_tod = st[7];
    g_manual = 1;
    return 1;
}

/* -------------------------------------------------------------- sounds --- */
/* Four waveOut voices (the kernel mixer blends them); tiny generated 16-bit
 * 22 kHz effects: 0=dig 1=place 2=step 3=jump. volume=0 disables. */

#define SND_HZ 22050
#define NSOUNDS 5                   /* dig place step jump hurt */
static struct { short *pcm; int len; } g_snd[NSOUNDS];
static struct { HWAVEOUT h; WAVEHDR hdr; int busy; } g_voice[4];
static int g_volume = 35;

static void gen_sounds(void)
{
    unsigned rng = 77777;
    for (int s = 0; s < NSOUNDS; s++) {
        int len = s == 0 ? SND_HZ / 11 : s == 1 ? SND_HZ / 28
                : s == 2 ? SND_HZ / 18 : s == 3 ? SND_HZ / 9
                : SND_HZ / 6;
        short *p = malloc(sizeof(short) * len);
        float lp = 0;
        for (int i = 0; i < len; i++) {
            float t = (float)i / len;
            float env = (1 - t) * (1 - t);
            float v = 0;
            rng = rng * 1664525u + 1013904223u;
            float n = ((int)(rng >> 16 & 0xffff) - 32768) / 32768.0f;
            switch (s) {
            case 0:                     /* dig: low rumbly noise */
                lp += (n - lp) * 0.18f;
                v = lp * 1.6f;
                break;
            case 1:                     /* place: woody click */
                v = (sinf(i * 2 * 3.14159f * 950 / SND_HZ) > 0 ? 0.5f : -0.5f)
                  + n * 0.15f;
                break;
            case 2:                     /* step: soft scuff */
                lp += (n - lp) * 0.35f;
                v = lp * 0.55f;
                break;
            case 3:                     /* jump: rising chirp */
                v = sinf(i * 2 * 3.14159f * (300 + 400 * t) / SND_HZ) * 0.4f;
                break;
            default:                    /* hurt: falling groan + thump */
                v = sinf(i * 2 * 3.14159f * (200 - 110 * t) / SND_HZ) * 0.55f
                  + n * 0.18f * (1 - t);
                break;
            }
            float out = v * env;
            if (out > 1) out = 1;
            if (out < -1) out = -1;
            p[i] = (short)(out * 30000);
        }
        g_snd[s].pcm = p;
        g_snd[s].len = len;
    }

    if (g_volume <= 0) return;
    WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 1, SND_HZ, SND_HZ * 2, 2, 16, 0 };
    DWORD v = (DWORD)(g_volume * 65535 / 100);
    for (int i = 0; i < 4; i++) {
        if (waveOutOpen(&g_voice[i].h, WAVE_MAPPER, &fmt, 0, 0,
                        CALLBACK_NULL) != MMSYSERR_NOERROR) {
            g_voice[i].h = NULL;
            continue;
        }
        waveOutSetVolume(g_voice[i].h, v | (v << 16));
    }
}

static void play_sound(int which)
{
    if (g_volume <= 0 || g_bench) return;
    for (int i = 0; i < 4; i++) {
        if (!g_voice[i].h) continue;
        if (g_voice[i].busy) {
            if (g_voice[i].hdr.dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(g_voice[i].h, &g_voice[i].hdr,
                                       sizeof(WAVEHDR));
                g_voice[i].busy = 0;
            } else
                continue;
        }
        memset(&g_voice[i].hdr, 0, sizeof(WAVEHDR));
        g_voice[i].hdr.lpData = (LPSTR)g_snd[which].pcm;
        g_voice[i].hdr.dwBufferLength = g_snd[which].len * 2;
        if (waveOutPrepareHeader(g_voice[i].h, &g_voice[i].hdr,
                                 sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
            waveOutWrite(g_voice[i].h, &g_voice[i].hdr,
                         sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
            g_voice[i].busy = 1;
        return;
    }
}

/* ------------------------------------------------------------- textures -- */

static unsigned g_rng = 0x12345678u;
static unsigned rng(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 16; }

static void tile_pixel(int tile, int x, int y, int *r, int *g, int *b)
{
    int n;
    switch (tile) {
    case T_GRASS_TOP:
        n = (int)(rng() % 50);
        *r = 60 + n / 3; *g = 130 + n; *b = 50 + n / 3;
        break;
    case T_GRASS_SIDE:
        n = (int)(rng() % 40);
        *r = 134 + n - 20; *g = 96 + n - 20; *b = 67 + n - 20;
        if (y < 8) {
            int gn = (int)(rng() % 50);
            *r = 60 + gn / 3; *g = 140 + gn; *b = 50 + gn / 3;
        }
        break;
    case T_DIRT:
        n = (int)(rng() % 40);
        *r = 134 + n - 20; *g = 96 + n - 20; *b = 67 + n - 20;
        break;
    case T_PLANKS:
        n = (int)(rng() % 24);
        *r = 168 + n - 12; *g = 130 + n - 12; *b = 78 + n - 12;
        if (y % 16 == 15) { *r -= 45; *g -= 40; *b -= 30; }
        if ((x + (y / 16) * 23) % 32 == 0) { *r -= 30; *g -= 25; *b -= 20; }
        break;
    case T_LOG_SIDE:
        n = (int)(rng() % 20);
        *r = 96 + n; *g = 70 + n; *b = 42 + n / 2;
        if ((x % 8) < 2) { *r -= 28; *g -= 22; *b -= 14; }    /* bark lines */
        break;
    case T_LOG_TOP: {
        int cx = x - 32, cy = y - 32;
        int d = (int)sqrtf((float)(cx * cx + cy * cy));
        n = (int)(rng() % 14);
        if ((d / 5) & 1) { *r = 150 + n; *g = 116 + n; *b = 66; }
        else             { *r = 116 + n; *g = 86 + n;  *b = 48; }
        break;
    }
    case T_LEAVES:
        n = (int)(rng() % 60);
        *r = 32 + n / 4; *g = 88 + n; *b = 30 + n / 5;
        break;
    case T_SAND:
        n = (int)(rng() % 30);
        *r = 212 + n - 15; *g = 198 + n - 15; *b = 150 + n - 15;
        break;
    case T_WATER:
        n = (int)(rng() % 24);
        *r = 30 + n / 2; *g = 70 + n / 2; *b = 170 + n;
        break;
    case T_COBBLE: {
        int gx = (x / 16) & 3, gy = (y / 16) & 3;
        n = (int)(rng() % 26);
        int base = 110 + ((gx * 7 + gy * 13) % 3) * 14;
        *r = *g = *b = base + n - 13;
        if (x % 16 < 2 || y % 16 < 2) { *r -= 35; *g -= 35; *b -= 35; }
        break;
    }
    case T_COAL:
        n = (int)(rng() % 35);
        *r = *g = *b = 110 + n - 17;
        if (((x / 6) * 31 + (y / 6) * 17) % 7 < 2 &&
            (rng() & 3)) { *r = *g = *b = 28 + (int)(rng() % 18); }
        break;
    default:                            /* T_STONE */
        n = (int)(rng() % 35);
        *r = *g = *b = 110 + n - 17;
        break;
    }
    if (x == 0 || y == 0) { *r -= 25; *g -= 25; *b -= 25; }
}

static int make_textures(void)
{
    for (int t = 0; t < NTILES; t++) {
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, TILE, TILE, 1, 0,
                       D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex[t], NULL)))
            return 0;
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DTexture9_LockRect(g_tex[t], 0, &lr, NULL, 0)))
            return 0;
        for (int y = 0; y < TILE; y++) {
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < TILE; x++) {
                int r, g, b;
                tile_pixel(t, x, y, &r, &g, &b);
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                row[x] = D3DCOLOR_ARGB(255, r, g, b);
            }
        }
        IDirect3DTexture9_UnlockRect(g_tex[t], 0);
    }
    return 1;
}

/* crack overlay: transparent texture with dark strands radiating outward */
static int make_crack(void)
{
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, TILE, TILE, 1, 0,
                   D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_crack, NULL)))
        return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_crack, 0, &lr, NULL, 0)))
        return 0;
    for (int y = 0; y < TILE; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < TILE; x++)
            row[x] = 0;
    }
    unsigned h = 0xC0FFEE;
    for (int s = 0; s < 9; s++) {       /* 9 jagged strands from center */
        float x = 32, y = 32;
        h = h * 1664525u + 1013904223u;
        float ang = (h >> 16 & 255) / 255.0f * 6.2832f;
        for (int i = 0; i < 26; i++) {
            h = h * 1664525u + 1013904223u;
            ang += ((int)(h >> 16 & 31) - 15.5f) * 0.04f;
            x += cosf(ang) * 1.3f;
            y += sinf(ang) * 1.3f;
            int ix = (int)x, iy = (int)y;
            if (ix < 0 || iy < 0 || ix >= TILE || iy >= TILE) break;
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + iy * lr.Pitch);
            row[ix] = D3DCOLOR_ARGB(210, 15, 12, 10);
            if (ix + 1 < TILE) row[ix + 1] = D3DCOLOR_ARGB(140, 15, 12, 10);
        }
    }
    IDirect3DTexture9_UnlockRect(g_crack, 0);
    return 1;
}

/* GDI-rendered fixed-width font atlas: chars 32..127, 16x16 cells, 256x128 */
static int make_font(void)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 256;
    bi.bmiHeader.biHeight = -128;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) { DeleteDC(dc); return 0; }
    SelectObject(dc, dib);
    HFONT font = CreateFontA(-15, 0, 0, 0, FW_BOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Courier New");
    SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT full = { 0, 0, 256, 128 };
    FillRect(dc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    for (int c = 32; c < 128; c++) {
        char ch = (char)c;
        TextOutA(dc, ((c - 32) % 16) * 16 + 2, ((c - 32) / 16) * 16, &ch, 1);
    }
    SIZE sz;
    GetTextExtentPoint32A(dc, "M", 1, &sz);
    g_glyph_w = sz.cx;
    GdiFlush();

    int ok = 0;
    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(g_dev, 256, 128, 1, 0,
                      D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_font, NULL))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_font, 0, &lr, NULL, 0))) {
            for (int y = 0; y < 128; y++) {
                DWORD *src = (DWORD *)bits + y * 256;
                DWORD *dst = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < 256; x++) {
                    int lum = src[x] & 0xFF;
                    dst[x] = ((DWORD)lum << 24) | 0x00FFFFFF;
                }
            }
            IDirect3DTexture9_UnlockRect(g_font, 0);
            ok = 1;
        }
    }
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(dc);
    return ok;
}

/* ----------------------------------------------------------------- HUD --- */

static void hud_begin(void)
{
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DESTBLEND,
                                    D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_COLOROP,
                                          D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(g_dev, HVTX_FVF);
}

static void hud_end(void)
{
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
}

static void hud_solidcolor(int on)
{
    /* NULL-texture draws must not sample stage-0's TEXTURE arg (undefined) */
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP,
            on ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2,
            D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAOP,
            on ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG2,
            D3DTA_DIFFUSE);
}

static void hud_quad(float x0, float y0, float x1, float y1, DWORD col,
                     IDirect3DTexture9 *tex, float u0, float v0,
                     float u1, float v1)
{
    HVTX q[6] = {
        { x0, y0, 0, 1, col, u0, v0 }, { x1, y0, 0, 1, col, u1, v0 },
        { x1, y1, 0, 1, col, u1, v1 }, { x0, y0, 0, 1, col, u0, v0 },
        { x1, y1, 0, 1, col, u1, v1 }, { x0, y1, 0, 1, col, u0, v1 },
    };
    if (!tex) hud_solidcolor(1);
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 2, q,
                                     sizeof(HVTX));
    if (!tex) hud_solidcolor(0);
}

static void draw_text(float x, float y, const char *s, DWORD col)
{
    static HVTX buf[6 * 128];
    int n = 0;
    float cx = x;
    for (; *s && n < 128; s++) {
        if (*s == '\n') { y += 18; cx = x; continue; }
        int c = (unsigned char)*s;
        if (c < 32 || c > 127) c = '?';
        c -= 32;
        float u0 = (c % 16) * 16.0f / 256.0f, v0 = (c / 16) * 16.0f / 128.0f;
        float u1 = u0 + 16.0f / 256.0f, v1 = v0 + 16.0f / 128.0f;
        HVTX q[6] = {
            { cx, y, 0, 1, col, u0, v0 },
            { cx + 16, y, 0, 1, col, u1, v0 },
            { cx + 16, y + 16, 0, 1, col, u1, v1 },
            { cx, y, 0, 1, col, u0, v0 },
            { cx + 16, y + 16, 0, 1, col, u1, v1 },
            { cx, y + 16, 0, 1, col, u0, v1 },
        };
        memcpy(&buf[n * 6], q, sizeof q);
        n++;
        cx += g_glyph_w;
    }
    if (!n) return;
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)g_font);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, n * 2, buf,
                                     sizeof(HVTX));
}

static void draw_hotbar(void)
{
    float slot = 46, pad = 5;
    float total = NHOTBAR * slot + (NHOTBAR - 1) * pad;
    float x = (g_win_w - total) / 2.0f;
    float y = g_win_h - slot - 12;
    for (int i = 0; i < NHOTBAR; i++) {
        DWORD frame = i == g_hotbar_idx ? D3DCOLOR_ARGB(230, 255, 255, 255)
                                        : D3DCOLOR_ARGB(140, 20, 20, 20);
        int have = g_inv[HOTBAR[i]];
        DWORD tint = (!g_walk || have > 0)
                   ? D3DCOLOR_ARGB(255, 255, 255, 255)
                   : D3DCOLOR_ARGB(255, 95, 95, 95);   /* out of stock */
        hud_quad(x - 3, y - 3, x + slot + 3, y + slot + 3, frame, NULL,
                 0, 0, 0, 0);
        hud_quad(x, y, x + slot, y + slot, tint,
                 g_tex[TILE_FOR[HOTBAR[i]][F_NORTH]], 0, 0, 1, 1);
        if (g_walk) {                   /* survival: show counts */
            char cnt[8];
            sprintf(cnt, "%d", have > 999 ? 999 : have);
            draw_text(x + slot - (float)strlen(cnt) * g_glyph_w - 1,
                      y + slot - 17, cnt,
                      D3DCOLOR_ARGB(255, 255, 255, 160));
        }
        x += slot + pad;
    }
    /* selected block name (+count) above the bar */
    char label[48];
    if (g_walk)
        sprintf(label, "%s x %d", BLOCK_NAME[g_sel], g_inv[g_sel]);
    else
        sprintf(label, "%s", BLOCK_NAME[g_sel]);
    float tx = (g_win_w - (float)strlen(label) * g_glyph_w) / 2.0f;
    draw_text(tx, y - 26, label, D3DCOLOR_ARGB(220, 255, 255, 255));
}

static void draw_health(void)
{
    if (!g_walk) return;                /* creative shows no vitals */
    float slot = 46, pad = 5;
    float total = NHOTBAR * slot + (NHOTBAR - 1) * pad;
    float x0 = (g_win_w - total) / 2.0f;
    float y = g_win_h - slot - 12 - 46;

    for (int i = 0; i < 10; i++) {      /* hearts: 2 hp each */
        float x = x0 + i * 17;
        int hp = g_health - i * 2;
        hud_quad(x, y, x + 14, y + 14, D3DCOLOR_ARGB(190, 25, 8, 8),
                 NULL, 0, 0, 0, 0);
        if (hp >= 2)
            hud_quad(x + 2, y + 2, x + 12, y + 12,
                     D3DCOLOR_ARGB(235, 210, 30, 30), NULL, 0, 0, 0, 0);
        else if (hp == 1)
            hud_quad(x + 2, y + 2, x + 7, y + 12,
                     D3DCOLOR_ARGB(235, 210, 30, 30), NULL, 0, 0, 0, 0);
    }

    if (g_air < 10.0f) {                /* bubbles while submerged */
        float by = y - 20;
        int full = (int)ceilf(g_air);
        for (int i = 0; i < 10; i++) {
            float x = x0 + i * 17;
            hud_quad(x + 2, by, x + 12, by + 10,
                     i < full ? D3DCOLOR_ARGB(220, 90, 160, 255)
                              : D3DCOLOR_ARGB(120, 20, 30, 50),
                     NULL, 0, 0, 0, 0);
        }
    }
}

static void draw_death(void)
{
    hud_quad(0, 0, (float)g_win_w, (float)g_win_h,
             D3DCOLOR_ARGB(150, 90, 0, 0), NULL, 0, 0, 0, 0);
    const char *t1 = "YOU DIED";
    const char *t2 = "CROSS / ENTER TO RESPAWN";
    draw_text((g_win_w - (float)strlen(t1) * g_glyph_w) / 2,
              g_win_h / 2.0f - 40, t1, D3DCOLOR_ARGB(255, 255, 220, 220));
    draw_text((g_win_w - (float)strlen(t2) * g_glyph_w) / 2,
              g_win_h / 2.0f + 4, t2, D3DCOLOR_ARGB(220, 230, 200, 200));
}

static void draw_crosshair(void)
{
    float cx = g_win_w / 2.0f, cy = g_win_h / 2.0f;
    DWORD c = D3DCOLOR_ARGB(200, 240, 240, 240);
    hud_quad(cx - 9, cy - 1, cx + 10, cy + 1, c, NULL, 0, 0, 0, 0);
    hud_quad(cx - 1, cy - 9, cx + 1, cy + 10, c, NULL, 0, 0, 0, 0);
}

static void draw_menu(void)
{
    static const char *ITEMS[3] = { "RESUME", "SAVE WORLD", "QUIT" };
    hud_quad(0, 0, (float)g_win_w, (float)g_win_h,
             D3DCOLOR_ARGB(150, 0, 0, 8), NULL, 0, 0, 0, 0);
    float cx = g_win_w / 2.0f, y = g_win_h / 2.0f - 90;
    draw_text(cx - 3 * g_glyph_w, y, "PAUSED",
              D3DCOLOR_ARGB(255, 255, 255, 255));
    y += 50;
    for (int i = 0; i < 3; i++) {
        char line[32];
        sprintf(line, "%s%s", i == g_menu_sel ? "> " : "  ", ITEMS[i]);
        draw_text(cx - 6 * g_glyph_w, y,
                  line, i == g_menu_sel ? D3DCOLOR_ARGB(255, 255, 230, 120)
                                        : D3DCOLOR_ARGB(220, 200, 200, 210));
        y += 30;
    }
}

/* --------------------------------------------------------------- input ---- */

static int g_mouse_reset = 1;

static void apply_range(void)
{
    float zf = g_range * 1.4f;
    if (zf < 120) zf = 120;
    D3DMATRIX proj = mat_perspective(1.22f, (float)g_win_w / g_win_h,
                                     0.3f, zf);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, &proj);
    union { float f; DWORD d; } fs = { g_range * 0.55f },
                                fe = { g_range * 0.98f };
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGSTART, fs.d);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGEND, fe.d);
}

static void menu_activate(void)
{
    if (g_menu_sel == 0) g_paused = 0;
    else if (g_menu_sel == 1) save_world();
    else { save_world(); DestroyWindow(g_hwnd); }
}

static void update_camera(float dt, float t)
{
    if (!g_manual && !g_bench) {
        g_yaw = t * 0.15f;
        g_pitch = 0.15f;
    }

    if (GetFocus() != g_hwnd) { g_mouse_reset = 1; return; }
    (void)dt;

    if (GetAsyncKeyState('W') & 0x8000) { g_manual = 1; g_move_f += 1; }
    if (GetAsyncKeyState('S') & 0x8000) { g_manual = 1; g_move_f -= 1; }
    if (GetAsyncKeyState('D') & 0x8000) { g_manual = 1; g_move_s += 1; }
    if (GetAsyncKeyState('A') & 0x8000) { g_manual = 1; g_move_s -= 1; }
    if (GetAsyncKeyState('E') & 0x8000) { g_manual = 1; g_move_u += 1; }
    if (GetAsyncKeyState('Q') & 0x8000) { g_manual = 1; g_move_u -= 1; }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
        g_manual = 1;
        g_jump = 1;
        g_move_u += 1;
    }
    if (GetAsyncKeyState(VK_OEM_4) & 0x8000) {
        if (g_range > 32) { g_range -= 1; apply_range(); }
    }
    if (GetAsyncKeyState(VK_OEM_6) & 0x8000) {
        if (g_range < 192) { g_range += 1; apply_range(); }
    }

    POINT center = { g_win_w / 2, g_win_h / 2 }, p;
    ClientToScreen(g_hwnd, &center);
    GetCursorPos(&p);
    if (g_mouse_reset) {
        g_mouse_reset = 0;
        SetCursorPos(center.x, center.y);
        return;
    }
    if (g_manual) {
        g_yaw   += (p.x - center.x) * 0.003f;
        g_pitch += (p.y - center.y) * 0.003f;
        if (g_pitch >  1.55f) g_pitch =  1.55f;
        if (g_pitch < -1.55f) g_pitch = -1.55f;
    }
    SetCursorPos(center.x, center.y);
}

/* ------------------------------------------------------------- gamepad --- */

static void hotbar_step(int dir)
{
    g_hotbar_idx = (g_hotbar_idx + dir + NHOTBAR) % NHOTBAR;
    g_sel = HOTBAR[g_hotbar_idx];
}

static void update_gamepad_mapped(float dt)
{
    PadState p;
    pad_poll(&p);
    if (!p.connected) return;

    static unsigned prev_cross, prev_start, prev_sq, prev_up, prev_down,
                    prev_l1, prev_r1;

    if (g_dead) {                       /* death screen: CROSS respawns */
        if (p.cross && !prev_cross) respawn();
        prev_cross = p.cross;
        return;
    }
    if (g_paused) {                     /* menu navigation */
        if (p.up && !prev_up)     g_menu_sel = (g_menu_sel + 2) % 3;
        if (p.down && !prev_down) g_menu_sel = (g_menu_sel + 1) % 3;
        if (p.cross && !prev_cross) menu_activate();
        if ((p.start && !prev_start) || (p.circle))
            g_paused = 0;
        prev_up = p.up; prev_down = p.down;
        prev_cross = p.cross; prev_start = p.start;
        return;
    }

    float strafe  = p.lx + (p.right ? 1.0f : 0) - (p.left ? 1.0f : 0);
    float forward = p.ly + (p.up ? 1.0f : 0) - (p.down ? 1.0f : 0);

    if (strafe != 0 || forward != 0 || p.rx != 0 || p.ry != 0 ||
        p.cross || p.circle || p.square || p.l2 || p.r2 || p.start) {
        g_manual = 1;
        g_pad_seen = 1;
    }

    g_move_s += strafe;
    g_move_f += forward;
    if (p.cross) {
        g_jump = 1;
        g_move_u += 1;
    }
    if (p.circle) g_move_u -= 1;

    if (p.cross && !prev_cross) {
        DWORD now = GetTickCount();
        static DWORD last_tap;
        if (now - last_tap < 300) {
            g_walk = !g_walk;
            g_vel_y = 0;
        }
        last_tap = now;
    }
    prev_cross = p.cross;

    g_yaw   += p.rx * 2.6f * dt;
    g_pitch -= p.ry * 2.6f * dt;
    if (g_pitch >  1.55f) g_pitch =  1.55f;
    if (g_pitch < -1.55f) g_pitch = -1.55f;

    if (p.r2) {
        g_break_held = 1;               /* survival digs via progress */
        if (!g_walk && g_click_cd <= 0) { edit_break(); g_click_cd = 0.22f; }
    }
    if (p.l2 && g_click_cd <= 0) {
        edit_place();
        g_click_cd = 0.22f;
    }

    static unsigned prev_tri;
    if (p.triangle && !prev_tri) craft_planks();
    prev_tri = p.triangle;

    if (p.square && !prev_sq) hotbar_step(1);
    if (p.l1 && !prev_l1) hotbar_step(-1);
    if (p.r1 && !prev_r1) hotbar_step(1);
    if (p.start && !prev_start) { g_paused = 1; g_menu_sel = 0; }
    prev_sq = p.square;
    prev_l1 = p.l1; prev_r1 = p.r1;
    prev_start = p.start;
    prev_up = p.up; prev_down = p.down;
}

static void update_gamepad(float dt)
{
    if (GetForegroundWindow() != g_hwnd) return;
    if (g_pad_mapped) {
        update_gamepad_mapped(dt);
        return;
    }
    /* unmapped fallback: raw twin-adapter heuristic (see tools/joyprobe.c) */
    JOYINFOEX j1 = { sizeof j1, JOY_RETURNALL };
    if (joyGetPosEx(1, &j1) != JOYERR_NOERROR || g_paused) return;
    float sx = ((int)j1.dwXpos - 32767) / 32767.0f;
    float sy = ((int)j1.dwYpos - 32767) / 32767.0f;
    if (sx > -0.25f && sx < 0.25f) sx = 0;
    if (sy > -0.25f && sy < 0.25f) sy = 0;
    if (sx || sy || j1.dwButtons) { g_manual = 1; g_pad_seen = 1; }
    g_move_s += sx;
    g_move_f -= sy;
    if (j1.dwButtons & 0x20) { g_move_u += 1; g_jump = 1; }
    if (j1.dwButtons & 0x10) g_move_u -= 1;
    if ((j1.dwButtons & 0x80) && g_click_cd <= 0) { edit_break(); g_click_cd = 0.22f; }
    if ((j1.dwButtons & 0x40) && g_click_cd <= 0) { edit_place(); g_click_cd = 0.22f; }
    (void)dt;
}

/* --------------------------------------------------------------- d3d9 ----- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (g_dead) {
            if (w == VK_RETURN || w == VK_SPACE) respawn();
            return 0;
        }
        if (g_paused) {
            if (w == VK_UP)     g_menu_sel = (g_menu_sel + 2) % 3;
            if (w == VK_DOWN)   g_menu_sel = (g_menu_sel + 1) % 3;
            if (w == VK_RETURN) menu_activate();
            if (w == VK_ESCAPE) g_paused = 0;
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (g_bench) DestroyWindow(h);
            else { g_paused = 1; g_menu_sel = 0; }
        }
        else if (w >= '1' && w <= '0' + NHOTBAR) {
            g_hotbar_idx = (int)(w - '1');
            g_sel = HOTBAR[g_hotbar_idx];
        }
        else if (w == 'F') { g_walk = !g_walk; g_vel_y = 0; }
        else if (w == 'C') craft_planks();
        else if (w == VK_F3) g_stats = !g_stats;
        else if (w == VK_F5) save_world();
        return 0;
    case WM_LBUTTONDOWN:
        if (!g_paused) { g_manual = 1; edit_break(); g_click_cd = 0.3f; }
        return 0;
    case WM_RBUTTONDOWN:
        if (!g_paused) { g_manual = 1; edit_place(); g_click_cd = 0.3f; }
        return 0;
    case WM_MOUSEWHEEL:
        if (!g_paused)
            hotbar_step(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
        return 0;
    case WM_SETFOCUS: ShowCursor(FALSE); return 0;
    case WM_KILLFOCUS: ShowCursor(TRUE); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static int init_d3d(HWND hwnd)
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return 0;

    ZeroMemory(&g_pp, sizeof g_pp);
    g_pp.Windowed              = TRUE;
    g_pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    g_pp.hDeviceWindow         = hwnd;
    g_pp.BackBufferWidth       = g_win_w;
    g_pp.BackBufferHeight      = g_win_h;
    g_pp.BackBufferFormat      = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    g_pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE;

    D3DCAPS9 caps;
    DWORD vp = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    if (SUCCEEDED(IDirect3D9_GetDeviceCaps(g_d3d, D3DADAPTER_DEFAULT,
                                           D3DDEVTYPE_HAL, &caps)) &&
        (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        vp = D3DCREATE_HARDWARE_VERTEXPROCESSING;
        g_hwvp = 1;
    }
    if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
                                       D3DDEVTYPE_HAL, hwnd, vp, &g_pp,
                                       &g_dev))) {
        if (!g_hwvp) return 0;
        g_hwvp = 0;
        if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
                       D3DDEVTYPE_HAL, hwnd,
                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev)))
            return 0;
    }

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER,
                                     D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER,
                                     D3DTEXF_POINT);

    /* stage 0 alpha = texture alpha x diffuse alpha (fonts, water) */
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAOP,
                                          D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG1,
                                          D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG2,
                                          D3DTA_DIFFUSE);

    /* stage 1 scales everything by daylight (TEXTUREFACTOR) */
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_COLOROP,
                                          D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_COLORARG1,
                                          D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_COLORARG2,
                                          D3DTA_TFACTOR);
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_ALPHAOP,
                                          D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_ALPHAARG1,
                                          D3DTA_CURRENT);

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGVERTEXMODE,
                                    D3DFOG_LINEAR);
    apply_range();

    return make_textures() && make_font() && make_crack();
}

/* ------------------------------------------------------------ day/night -- */

static void update_daylight(float dt)
{
    if (!g_tod_frozen && !g_paused) {
        g_tod += dt / DAY_SECONDS;
        if (g_tod >= 1) g_tod -= 1;
    }
    /* 0.0-0.45 day, 0.45-0.55 dusk, 0.55-0.95 night, 0.95-1.0 dawn */
    float L;
    if (g_tod < 0.45f)      L = 1.0f;
    else if (g_tod < 0.55f) L = 1.0f - (g_tod - 0.45f) * 10.0f * 0.78f;
    else if (g_tod < 0.95f) L = 0.22f;
    else                    L = 0.22f + (g_tod - 0.95f) * 20.0f * 0.78f;
    g_daylight = L;

    float k = (L - 0.22f) / 0.78f;
    int r = (int)(8 + (135 - 8) * k);
    int g = (int)(10 + (206 - 10) * k);
    int b = (int)(28 + (235 - 28) * k);
    g_sky_color = D3DCOLOR_XRGB(r, g, b);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGCOLOR, g_sky_color);
    int li = (int)(L * 255);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_TEXTUREFACTOR,
                                    D3DCOLOR_ARGB(255, li, li, li));
}

/* -------------------------------------------------------------- render --- */

static int g_drawn_tris, g_drawn_chunks;
float g_fps;                        /* shared with the stats HUD */

static void render_frame(void)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           g_sky_color, 1.0f, 0);
    g_drawn_tris = 0;
    g_drawn_chunks = 0;

    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        /* re-arm the daylight stage (HUD disables it at the end of a frame) */
        IDirect3DDevice9_SetTextureStageState(g_dev, 1, D3DTSS_COLOROP,
                                              D3DTOP_MODULATE);
        D3DMATRIX world = mat_identity();
        D3DMATRIX view  = mat_view();
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &world);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &view);

        D3DMATRIX proj;
        IDirect3DDevice9_GetTransform(g_dev, D3DTS_PROJECTION, &proj);
        D3DMATRIX vp = mat_mul(&view, &proj);
        frustum_from(&vp);

        static CHUNKMESH *vis[WORLD_CX * WORLD_CZ];
        int nvis = 0;
        float margin = CHUNK * 0.71f;
        for (int cz = 0; cz < WORLD_CZ; cz++)
            for (int cx = 0; cx < WORLD_CX; cx++) {
                CHUNKMESH *c = &g_chunks[cz][cx];
                if (!c->vb || !c->ntris) continue;
                float dx = (cx * CHUNK + 8) - g_cam_x;
                float dz = (cz * CHUNK + 8) - g_cam_z;
                if (sqrtf(dx * dx + dz * dz) > g_range + margin) continue;
                if (!aabb_visible(c->min, c->max)) continue;
                vis[nvis++] = c;
            }
        g_drawn_chunks = nvis;

        IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);

        /* opaque pass */
        for (int t = 0; t < NTILES; t++) {
            if (t == T_WATER) continue;
            IDirect3DDevice9_SetTexture(g_dev, 0,
                                        (IDirect3DBaseTexture9 *)g_tex[t]);
            for (int i = 0; i < nvis; i++) {
                CHUNKMESH *c = vis[i];
                if (!c->prims[t]) continue;
                IDirect3DDevice9_SetStreamSource(g_dev, 0, c->vb, 0,
                                                 sizeof(VTX));
                IDirect3DDevice9_SetIndices(g_dev, c->ib);
                IDirect3DDevice9_DrawIndexedPrimitive(g_dev,
                        D3DPT_TRIANGLELIST, 0, 0, c->nverts,
                        c->ib_start[t], c->prims[t]);
                g_drawn_tris += c->prims[t];
            }
        }

        /* water pass: blended, no depth writes */
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SRCBLEND,
                                        D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DESTBLEND,
                                        D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZWRITEENABLE, FALSE);
        IDirect3DDevice9_SetTexture(g_dev, 0,
                                    (IDirect3DBaseTexture9 *)g_tex[T_WATER]);
        for (int i = 0; i < nvis; i++) {
            CHUNKMESH *c = vis[i];
            if (!c->prims[T_WATER]) continue;
            IDirect3DDevice9_SetStreamSource(g_dev, 0, c->vb, 0, sizeof(VTX));
            IDirect3DDevice9_SetIndices(g_dev, c->ib);
            IDirect3DDevice9_DrawIndexedPrimitive(g_dev, D3DPT_TRIANGLELIST,
                    0, 0, c->nverts, c->ib_start[T_WATER],
                    c->prims[T_WATER]);
            g_drawn_tris += c->prims[T_WATER];
        }
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZWRITEENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);

        /* particles: camera-facing quads, grouped per tile texture */
        {
            float rxv = cosf(g_yaw) * 0.09f, rzv = -sinf(g_yaw) * 0.09f;
            IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
            for (int t = 0; t < NTILES; t++) {
                static VTX pv[NPART * 6];
                int n = 0;
                for (int i = 0; i < NPART; i++) {
                    if (g_part[i].life <= 0 || g_part[i].tile != t)
                        continue;
                    float px = g_part[i].x, py = g_part[i].y,
                          pz = g_part[i].z;
                    DWORD col = D3DCOLOR_ARGB(255, 220, 220, 220);
                    VTX q[6] = {
                        { px - rxv, py + 0.09f, pz - rzv, col, 0.1f, 0.1f },
                        { px + rxv, py + 0.09f, pz + rzv, col, 0.4f, 0.1f },
                        { px + rxv, py - 0.09f, pz + rzv, col, 0.4f, 0.4f },
                        { px - rxv, py + 0.09f, pz - rzv, col, 0.1f, 0.1f },
                        { px + rxv, py - 0.09f, pz + rzv, col, 0.4f, 0.4f },
                        { px - rxv, py - 0.09f, pz - rzv, col, 0.1f, 0.4f },
                    };
                    memcpy(&pv[n * 6], q, sizeof q);
                    n++;
                }
                if (!n) continue;
                IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE,
                                                D3DCULL_NONE);
                IDirect3DDevice9_SetTexture(g_dev, 0,
                        (IDirect3DBaseTexture9 *)g_tex[t]);
                IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST,
                        n * 2, pv, sizeof(VTX));
                IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE,
                                                D3DCULL_CW);
            }
        }

        /* crack overlay on the block being dug */
        if (g_break_on && g_break_prog > 0.02f) {
            VTX cv[36];
            int bx = g_break_cell[0], by = g_break_cell[1],
                bz = g_break_cell[2];
            DWORD col = D3DCOLOR_ARGB((int)(g_break_prog * 255),
                                      255, 255, 255);
            float e = 0.006f;
            int vi = 0;
            for (int f = 0; f < 6; f++) {
                float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                VTX c4[4];
                for (int k = 0; k < 4; k++) {
                    const BYTE *o = FACES[f].c[k];
                    c4[k].x = bx + (o[0] ? 1 + e : -e);
                    c4[k].y = by + (o[1] ? 1 + e : -e);
                    c4[k].z = bz + (o[2] ? 1 + e : -e);
                    c4[k].color = col;
                    c4[k].u = uv[k][0];
                    c4[k].v = uv[k][1];
                }
                cv[vi++] = c4[0]; cv[vi++] = c4[1]; cv[vi++] = c4[2];
                cv[vi++] = c4[0]; cv[vi++] = c4[2]; cv[vi++] = c4[3];
            }
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE,
                                            TRUE);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZWRITEENABLE,
                                            FALSE);
            IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
            IDirect3DDevice9_SetTexture(g_dev, 0,
                    (IDirect3DBaseTexture9 *)g_crack);
            IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 12,
                                             cv, sizeof(VTX));
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZWRITEENABLE, TRUE);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE,
                                            FALSE);
        }

        /* block highlight */
        int hit[3], prev[3];
        if (!g_bench && !g_paused && raycast(hit, prev)) {
            typedef struct { float x, y, z; DWORD c; } LVTX;
            float e = 0.004f;
            float x0 = hit[0] - e, x1 = hit[0] + 1 + e;
            float y0 = hit[1] - e, y1 = hit[1] + 1 + e;
            float z0 = hit[2] - e, z1 = hit[2] + 1 + e;
            DWORD bc = D3DCOLOR_XRGB(20, 20, 20);
            LVTX ln[24] = {
                {x0,y0,z0,bc},{x1,y0,z0,bc}, {x1,y0,z0,bc},{x1,y0,z1,bc},
                {x1,y0,z1,bc},{x0,y0,z1,bc}, {x0,y0,z1,bc},{x0,y0,z0,bc},
                {x0,y1,z0,bc},{x1,y1,z0,bc}, {x1,y1,z0,bc},{x1,y1,z1,bc},
                {x1,y1,z1,bc},{x0,y1,z1,bc}, {x0,y1,z1,bc},{x0,y1,z0,bc},
                {x0,y0,z0,bc},{x0,y1,z0,bc}, {x1,y0,z0,bc},{x1,y1,z0,bc},
                {x1,y0,z1,bc},{x1,y1,z1,bc}, {x0,y0,z1,bc},{x0,y1,z1,bc},
            };
            IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
            IDirect3DDevice9_SetFVF(g_dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
            IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_LINELIST, 12, ln,
                                             sizeof(LVTX));
        }

        /* first-person hand: held block, swings on dig/place, bobs on walk */
        if (!g_bench && !g_paused) {
            float sw = sinf(g_swing * 3.14159f);      /* 0..1..0 */
            D3DMATRIX sc = mat_identity();
            sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = 0.34f;
            D3DMATRIX rx = mat_rot_x(-0.30f - sw * 0.85f);
            D3DMATRIX ry = mat_rot_y(0.5f + sw * 0.3f);
            D3DMATRIX tr = mat_translate(
                0.55f - sw * 0.15f,
                -0.52f - sw * 0.12f + sinf(g_bob) * 0.02f,
                1.05f - sw * 0.08f);
            D3DMATRIX m = mat_mul(&sc, &rx);
            m = mat_mul(&m, &ry);
            m = mat_mul(&m, &tr);
            D3DMATRIX ident = mat_identity();
            IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &m);
            IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &ident);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
                                            D3DZB_FALSE);

            VTX hv[36];
            int vi = 0;
            for (int f = 0; f < 6; f++) {
                int s = 255 * FACES[f].shade / 100;
                DWORD col = D3DCOLOR_ARGB(255, s, s, s);
                float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                VTX c4[4];
                for (int k = 0; k < 4; k++) {
                    const BYTE *o = FACES[f].c[k];
                    c4[k].x = o[0] - 0.5f;
                    c4[k].y = o[1] - 0.5f;
                    c4[k].z = o[2] - 0.5f;
                    c4[k].color = col;
                    c4[k].u = uv[k][0];
                    c4[k].v = uv[k][1];
                }
                hv[vi++] = c4[0]; hv[vi++] = c4[1]; hv[vi++] = c4[2];
                hv[vi++] = c4[0]; hv[vi++] = c4[2]; hv[vi++] = c4[3];
            }
            IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
            IDirect3DDevice9_SetTexture(g_dev, 0,
                    (IDirect3DBaseTexture9 *)g_tex[TILE_FOR[g_sel][F_NORTH]]);
            IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 12,
                                             hv, sizeof(VTX));
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
                                            D3DZB_TRUE);
        }

        /* ------------------------------------------------------- HUD ----- */
        if (!g_bench) {
            hud_begin();
            if (g_hurt_t > 0)           /* red damage flash */
                hud_quad(0, 0, (float)g_win_w, (float)g_win_h,
                         D3DCOLOR_ARGB((int)(g_hurt_t / 0.35f * 110),
                                       200, 0, 0), NULL, 0, 0, 0, 0);
            draw_crosshair();
            draw_hotbar();
            draw_health();
            if (g_stats) {
                extern float g_fps;
                static char line[160];
                /* clock: tod 0 = 06:00 sunrise, so light matches the label */
                float clk = g_tod * 24 + 6;
                if (clk >= 24) clk -= 24;
                int hh = (int)clk, mm = (int)(clk * 60) % 60;
                sprintf(line, "FPS %.0f  R%.0f  TRIS %d  CH %d  %s  "
                        "X%.0f Y%.0f Z%.0f  %02d:%02d",
                        g_fps, g_range, g_drawn_tris,
                        g_drawn_chunks, g_walk ? "SURVIVAL" : "CREATIVE",
                        g_cam_x, g_cam_y, g_cam_z, hh, mm);
                draw_text(8, 6, line, D3DCOLOR_ARGB(200, 255, 255, 255));
            }
            if (g_toast_t > 0) {
                float tx = (g_win_w - (float)strlen(g_toast) * g_glyph_w) / 2;
                draw_text(tx, g_win_h * 0.32f, g_toast,
                          D3DCOLOR_ARGB(240, 255, 240, 150));
            }
            if (g_paused) draw_menu();
            if (g_dead) draw_death();
            hud_end();
        }

        IDirect3DDevice9_EndScene(g_dev);
    }

    HRESULT hr = IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST &&
        IDirect3DDevice9_TestCooperativeLevel(g_dev) == D3DERR_DEVICENOTRESET)
        IDirect3DDevice9_Reset(g_dev, &g_pp);
}

/* draw a bare text frame (used during world gen before the loop starts) */
static void loading_frame(const char *msg)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(10, 12, 30), 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        hud_begin();
        float tx = (g_win_w - (float)strlen(msg) * g_glyph_w) / 2;
        draw_text(tx, g_win_h / 2.0f - 8, msg,
                  D3DCOLOR_ARGB(255, 230, 230, 240));
        hud_end();
        IDirect3DDevice9_EndScene(g_dev);
    }
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
}

/* --------------------------------------------------------------- bench ---- */

static void bench_start(double now)
{
    g_range = BENCH_RANGES[g_bench_idx];
    apply_range();
    g_bench_warm = 1;
    g_bench_t0 = now;
    g_bench_frames = 0;
}

static int bench_step(double now)
{
    double el = now - g_bench_t0;
    if (g_bench_warm) {
        /* wait for the mesh queue to drain (chunk streaming), cap 25s */
        if ((!queue_busy() && el >= 1.0) || el >= 25.0) {
            g_bench_warm = 0;
            g_bench_t0 = now;
            g_bench_frames = 0;
        }
        return 1;
    }
    g_bench_frames++;
    if (el < 8.0) return 1;

    char line[128];
    sprintf(line, "range=%3.0f  avg_fps=%6.1f  tris=%6d  chunks=%3d\r\n",
            g_range, g_bench_frames / el, g_drawn_tris, g_drawn_chunks);
    strcat(g_bench_log, line);

    if (++g_bench_idx >= NBENCH) {
        FILE *fp = fopen("bench.txt", "wb");
        if (fp) {
            fprintf(fp, "xp-craft bench  %dx%d  vp=%s  world=%dx%dx%d\r\n",
                    g_win_w, g_win_h, g_hwvp ? "hardware" : "software",
                    WX, WZ, WORLD_H);
            fputs(g_bench_log, fp);
            fclose(fp);
        }
        return 0;
    }
    bench_start(now);
    return 1;
}

/* ---------------------------------------------------------------- main ---- */

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev;
    g_bench = cmdline && strstr(cmdline, "bench") ? 1 : 0;
    g_windowed = cmdline && strstr(cmdline, "windowed") ? 1 : 0;
    const char *ta = cmdline ? strstr(cmdline, "time=") : NULL;
    float arg_tod = ta ? (float)atof(ta + 5) : 0;
    if (ta) { g_tod = arg_tod; g_tod_frozen = 1; }
    const char *va = cmdline ? strstr(cmdline, "volume=") : NULL;
    if (va) g_volume = atoi(va + 7);
    const char *aa = cmdline ? strstr(cmdline, "at=") : NULL;
    if (g_bench) { g_tod = 0.2f; g_tod_frozen = 1; }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "xpcraft";
    RegisterClassA(&wc);

    DWORD style;
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww, wh;
    if (g_windowed || g_bench) {
        g_win_w = 800; g_win_h = 600;
        RECT r = { 0, 0, g_win_w, g_win_h };
        style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRect(&r, style, FALSE);
        ww = r.right - r.left; wh = r.bottom - r.top;
    } else {
        g_win_w = GetSystemMetrics(SM_CXSCREEN);
        g_win_h = GetSystemMetrics(SM_CYSCREEN);
        style = WS_POPUP;
        wx = wy = 0;
        ww = g_win_w; wh = g_win_h;
    }
    g_hwnd = CreateWindowA("xpcraft", "xp-craft", style, wx, wy, ww, wh,
                           NULL, NULL, hinst, NULL);
    if (!init_d3d(g_hwnd)) {
        MessageBoxA(NULL, "D3D9 init failed", "xp-craft", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, show);
    update_daylight(0);

    g_world = calloc(1, (size_t)WORLD_H * WZ * WX);
    if (!g_world) return 1;

    world_file_path();
    g_pad_mapped = pad_load("C:\\XP_Share\\pad.cfg");
    gen_sounds();

    loading_frame("GENERATING WORLD...");
    if (!load_world()) {
        gen_world();
        /* spawn on dry land near the center */
        int sx = WX / 2, sz = WZ / 2;
        for (int tries = 0; tries < 4000; tries++) {
            int tx = sx + (int)(hash2u(tries, 7) % 200) - 100;
            int tz = sz + (int)(hash2u(tries, 91) % 200) - 100;
            int h = terrain_h(tx, tz);
            if (h > WATER_Y + 1 &&
                g_world[h - 1][tz][tx] == B_GRASS &&
                g_world[h][tz][tx] == B_AIR) {
                g_cam_x = tx + 0.5f;
                g_cam_z = tz + 0.5f;
                g_cam_y = h + EYE_H + 0.01f;
                break;
            }
        }
        if (g_cam_x == 0) {
            g_cam_x = sx + 0.5f; g_cam_z = sz + 0.5f;
            g_cam_y = terrain_h(sx, sz) + EYE_H + 2.0f;
        }
        g_pitch = 0.15f;
        g_spawn_x = g_cam_x;            /* world spawn = first stand */
        g_spawn_y = g_cam_y;
        g_spawn_z = g_cam_z;
    }
    if (g_bench) g_walk = 0;
    if (ta) g_tod = arg_tod;        /* time= wins over the saved time */
    if (aa) {                       /* dev teleport: at=x,z */
        float ax = 0, az = 0;
        if (sscanf(aa + 3, "%f,%f", &ax, &az) == 2) {
            g_cam_x = ax; g_cam_z = az;
            g_cam_y = 46.0f;
            g_walk = 0;
            g_manual = 1;
            g_pitch = 0.5f;
        }
    }

    loading_frame("BUILDING MESHES...");
    InitializeCriticalSection(&g_qlock);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);

    LARGE_INTEGER freq, start, t0, now, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    t0 = prev_t = start;
    int frames = 0;
    float fps = 0;

    if (g_bench) bench_start(0.0);

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev_t.QuadPart)
                           / (double)freq.QuadPart);
        double td = (double)(now.QuadPart - start.QuadPart)
                  / (double)freq.QuadPart;
        prev_t = now;

        if (g_bench) {
            g_yaw = (float)(td * (2.0 * 3.14159265 / 8.0));
            g_pitch = 0.15f;
        } else {
            g_click_cd -= dt;
            g_move_s = g_move_f = g_move_u = 0;
            g_jump = 0;
            g_break_held = 0;
            if (!g_paused) update_camera(dt, (float)td);
            update_gamepad(dt);
            if (!g_paused && !g_dead) {
                if (dt > 0.1f) dt = 0.1f;
                if (GetFocus() == g_hwnd &&
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                    g_break_held = 1;
                    if (!g_walk && g_click_cd <= 0) {
                        edit_break();
                        g_click_cd = 0.22f;
                    }
                }
                if (GetFocus() == g_hwnd &&
                    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) &&
                    g_click_cd <= 0) {
                    edit_place();
                    g_click_cd = 0.22f;
                }
                if (g_break_held) swing_kick();
                if (g_swing_on) {       /* hand swing animation */
                    g_swing += dt / 0.3f;
                    if (g_swing >= 1) {
                        if (g_break_held) g_swing = 0;   /* keep chopping */
                        else { g_swing_on = 0; g_swing = 0; }
                    }
                }
                update_breaking(dt, g_break_held);
                update_particles(dt);
                step_player(dt);
                update_survival(dt);
                g_autosave_t += dt;
                if (g_autosave_t > AUTOSAVE_S) {
                    g_autosave_t = 0;
                    save_world();
                }
            }
        }

        update_daylight(dt);
        if (g_toast_t > 0) g_toast_t -= dt;
        else g_toast[0] = 0;
        if (g_hurt_t > 0) g_hurt_t -= dt;

        stream_chunks();
        render_frame();
        frames++;

        if (g_bench && !bench_step(td)) break;

        double sec = (double)(now.QuadPart - t0.QuadPart)
                   / (double)freq.QuadPart;
        if (sec >= 1.0) {
            fps = (float)(frames / sec);
            g_fps = fps;
            char title[64];
            sprintf(title, "xp-craft - %.0f FPS", fps);
            SetWindowTextA(g_hwnd, title);
            frames = 0;
            t0 = now;
        }
    }

    if (!g_bench) save_world();

    g_worker_run = 0;
    SetEvent(g_wake);
    WaitForSingleObject(g_worker, 2000);
    CloseHandle(g_worker);
    CloseHandle(g_wake);
    DeleteCriticalSection(&g_qlock);

    for (int cz = 0; cz < WORLD_CZ; cz++)
        for (int cx = 0; cx < WORLD_CX; cx++) {
            CHUNKMESH *c = &g_chunks[cz][cx];
            if (c->vb) IDirect3DVertexBuffer9_Release(c->vb);
            if (c->ib) IDirect3DIndexBuffer9_Release(c->ib);
            free(c->pverts);
            free(c->pidx);
        }
    for (int t = 0; t < NTILES; t++)
        if (g_tex[t]) IDirect3DTexture9_Release(g_tex[t]);
    if (g_font) IDirect3DTexture9_Release(g_font);
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    for (int i = 0; i < 4; i++)
        if (g_voice[i].h) waveOutClose(g_voice[i].h);
    free(g_world);
    return 0;
}
