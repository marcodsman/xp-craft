/* xp-craft milestone 4 — break/place blocks, raycast picking, save/load.
 *
 * On top of the M3 chunk-grid renderer: Amanatides-Woo voxel raycast from
 * the crosshair (reach 6), LMB breaks / RMB places with hold-repeat, edits
 * rebuild the touched chunk (+ neighbor if on a border) synchronously,
 * targeted block gets a wireframe highlight. Keys 1-4 pick grass/dirt/
 * stone/planks. World persists to world.dat next to the exe: loaded at
 * startup, saved on exit and on F5. y=0 is bedrock (unbreakable).
 *
 * 20x20 chunks (320x320x16 blocks). Each chunk greedy-meshes into one
 * indexed vertex buffer with draw ranges grouped by texture (4 separate
 * 64x64 wrap-addressed textures now — greedy quads need UV tiling, which
 * an atlas can't do in fixed-function). Chunks are culled by view range,
 * then by frustum (Gribb-Hartmann planes, AABB p-vertex test). Linear
 * vertex fog hides the pop at the range edge, Minecraft-style.
 *
 * Vertex processing: hardware if the driver reports HW T&L, else software.
 *
 * Modes:
 *   xp-craft.exe          normal: spawn at world center, slow turntable;
 *                         WASD/E/Q+mouse = manual fly, [ ] = view range, ESC quits
 *   xp-craft.exe bench    scripted 360-degree pan at eye level, 8s per view
 *                         range {32,48,64,96,128}, writes bench.txt and exits
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define WIN_W 800
#define WIN_H 600

#define CHUNK    16
#define WORLD_CX 20                 /* chunks along x */
#define WORLD_CZ 20                 /* chunks along z */
#define WORLD_H  16                 /* blocks tall (one chunk layer) */
#define WX (WORLD_CX * CHUNK)       /* world size in blocks */
#define WZ (WORLD_CZ * CHUNK)

#define TILE 64                     /* texture size in pixels */
#define NTILES 5
#define REACH 6.0f                  /* block interaction distance */

/* worst case emitted quads per chunk = 3D checkerboard: half the blocks
 * solid, all 6 faces each */
#define MAX_QUADS (CHUNK * CHUNK * CHUNK / 2 * 6)

enum { B_AIR, B_GRASS, B_DIRT, B_STONE, B_PLANKS, NBLOCKS };
enum { F_TOP, F_BOTTOM, F_NORTH, F_SOUTH, F_WEST, F_EAST };
enum { T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_PLANKS };

typedef struct { float x, y, z; DWORD color; float u, v; } VTX;
#define VTX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9  *ib;
    int ib_start[NTILES];           /* first index per texture group */
    int prims[NTILES];              /* triangle count per texture group */
    int nverts, ntris;
    float min[3], max[3];           /* AABB for frustum test */
} CHUNKMESH;

static IDirect3D9        *g_d3d;
static IDirect3DDevice9  *g_dev;
static IDirect3DTexture9 *g_tex[NTILES];
static D3DPRESENT_PARAMETERS g_pp;
static CHUNKMESH g_chunks[WORLD_CZ][WORLD_CX];
static BYTE (*g_world)[WZ][WX];     /* [y][z][x], malloc'd */
static int  g_running = 1;
static int  g_hwvp;                 /* 1 = hardware vertex processing */
static int  g_world_tris, g_mesh_ms;
static HWND g_hwnd;

static float g_range = 64.0f;       /* view range in blocks */
static const DWORD FOG_COLOR = D3DCOLOR_XRGB(0x87, 0xCE, 0xEB);

/* fly camera (turntable until first movement key) */
static float g_cam_x, g_cam_y, g_cam_z;
static float g_yaw, g_pitch;
static int   g_manual;

/* block editing */
static int   g_sel = B_PLANKS;      /* block type to place (keys 1-4) */
static int   g_remesh_ms;           /* last edit's rebuild cost */
static float g_click_cd;            /* hold-repeat cooldown */
static char  g_world_file[MAX_PATH];
static const char *BLOCK_NAME[NBLOCKS] =
    { "air", "grass", "dirt", "stone", "planks" };

/* bench mode */
static int    g_bench;
static int    g_bench_idx;          /* which range is being measured */
static int    g_bench_warm;         /* still in warmup second */
static double g_bench_t0, g_bench_frames;
static const float BENCH_RANGES[] = { 32, 48, 64, 96, 128 };
#define NBENCH ((int)(sizeof BENCH_RANGES / sizeof BENCH_RANGES[0]))
static char   g_bench_log[1024];

/* ---------------------------------------------------------------- mat4 --- */
/* D3D row-vector convention: v' = v * M, D3DMATRIX.m[row][col]. */

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
/* Plane extraction for row-vector clip = v*(view*proj): planes live in the
 * matrix COLUMNS (Gribb-Hartmann transposed). Inside: ax+by+cz+d >= 0. */

static float g_planes[6][4];

static void frustum_from(const D3DMATRIX *vp)
{
    for (int i = 0; i < 4; i++) {
        g_planes[0][i] = vp->m[i][3] + vp->m[i][0];   /* left   */
        g_planes[1][i] = vp->m[i][3] - vp->m[i][0];   /* right  */
        g_planes[2][i] = vp->m[i][3] + vp->m[i][1];   /* bottom */
        g_planes[3][i] = vp->m[i][3] - vp->m[i][1];   /* top    */
        g_planes[4][i] = vp->m[i][2];                 /* near (D3D z>=0) */
        g_planes[5][i] = vp->m[i][3] - vp->m[i][2];   /* far    */
    }
}

static int aabb_visible(const float min[3], const float max[3])
{
    for (int p = 0; p < 6; p++) {
        const float *pl = g_planes[p];
        float x = pl[0] >= 0 ? max[0] : min[0];   /* positive vertex */
        float y = pl[1] >= 0 ? max[1] : min[1];
        float z = pl[2] >= 0 ? max[2] : min[2];
        if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0)
            return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- world ---- */

static int terrain_h(int x, int z)
{
    float h = 7.0f
            + 4.0f * sinf(x * 0.055f) * cosf(z * 0.045f)     /* rolling hills */
            + 3.0f * sinf(x * 0.35f) * cosf(z * 0.28f) * 0.6f /* bumps */
            + 2.0f * sinf((x + z) * 0.11f);
    int hi = (int)h;
    if (hi < 1) hi = 1;
    if (hi > WORLD_H - 2) hi = WORLD_H - 2;
    return hi;
}

static int block_at(int x, int y, int z)
{
    if (y < 0) return B_STONE;      /* solid below the world: no bottom skirt */
    if (x < 0 || z < 0 || x >= WX || z >= WZ || y >= WORLD_H) return B_AIR;
    return g_world[y][z][x];
}

static void gen_world(void)
{
    g_world = calloc(1, sizeof(BYTE) * WORLD_H * WZ * WX);
    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) {
            int h = terrain_h(x, z);
            for (int y = 0; y < h; y++) {
                if      (y == h - 1) g_world[y][z][x] = B_GRASS;
                else if (y >= h - 4) g_world[y][z][x] = B_DIRT;
                else                 g_world[y][z][x] = B_STONE;
            }
        }
}

/* ------------------------------------------------------------- meshing --- */
/* Corner offsets keep the winding proven in M1/M2: a=uv(0,0) b=(w,0) c=(w,h)
 * d=(0,h), triangles abc+acd, CULL_CW. For a greedy rect spanning inclusive
 * block bounds [x0..x1][y0..y1][z0..z1], each corner component is: offset 0
 * -> min bound, offset 1 -> max bound + 1. That one rule works for all six
 * faces with the same offset table as the naive mesher. */

static const struct {
    int dx, dy, dz;
    BYTE c[4][3];                   /* corner offsets a,b,c,d (0=min,1=max+1) */
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
    [B_PLANKS] = { T_PLANKS, T_PLANKS, T_PLANKS, T_PLANKS,
                   T_PLANKS, T_PLANKS },
};

typedef struct { BYTE tile, face; BYTE x0, x1, y0, y1, z0, z1; } QUAD;
static QUAD g_quads[MAX_QUADS];     /* scratch, reused per chunk */
static int  g_nquads;

/* greedy-rectangle a 2D mask: mask[v][u], dims nu x nv; emit() gets the rect */
static void greedy_2d(BYTE *mask, int nu, int nv,
                      void (*emit)(int u0, int v0, int w, int h, int tile,
                                   void *ctx), void *ctx)
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
    EMITCTX *e = vctx;
    QUAD *q = &g_quads[g_nquads++];
    q->tile = (BYTE)tile;
    q->face = (BYTE)e->face;
    switch (e->face) {
    case F_TOP: case F_BOTTOM:      /* mask u=x, v=z, slice=y */
        q->x0 = u0; q->x1 = u0 + w - 1;
        q->z0 = v0; q->z1 = v0 + h - 1;
        q->y0 = q->y1 = e->slice;
        break;
    case F_NORTH: case F_SOUTH:     /* mask u=x, v=y, slice=z */
        q->x0 = u0; q->x1 = u0 + w - 1;
        q->y0 = v0; q->y1 = v0 + h - 1;
        q->z0 = q->z1 = e->slice;
        break;
    default:                        /* W/E: mask u=z, v=y, slice=x */
        q->z0 = u0; q->z1 = u0 + w - 1;
        q->y0 = v0; q->y1 = v0 + h - 1;
        q->x0 = q->x1 = e->slice;
    }
}

/* uv extents (in tiles) for a quad, per face group */
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

static void mesh_chunk_quads(int cx, int cz)
{
    static BYTE mask[CHUNK * CHUNK];
    int bx = cx * CHUNK, bz = cz * CHUNK;
    g_nquads = 0;
    EMITCTX e;

    /* TOP/BOTTOM: slice y, mask[z][x] */
    for (int f = F_TOP; f <= F_BOTTOM; f++)
        for (int y = 0; y < WORLD_H; y++) {
            int any = 0;
            for (int z = 0; z < CHUNK; z++)
                for (int x = 0; x < CHUNK; x++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        block_at(bx + x, y + FACES[f].dy, bz + z) == B_AIR)
                        t = TILE_FOR[blk][f] + 1;
                    mask[z * CHUNK + x] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = y;
            greedy_2d(mask, CHUNK, CHUNK, emit_rect, &e);
        }

    /* NORTH/SOUTH: slice z, mask[y][x] */
    for (int f = F_NORTH; f <= F_SOUTH; f++)
        for (int z = 0; z < CHUNK; z++) {
            int any = 0;
            for (int y = 0; y < WORLD_H; y++)
                for (int x = 0; x < CHUNK; x++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        block_at(bx + x, y, bz + z + FACES[f].dz) == B_AIR)
                        t = TILE_FOR[blk][f] + 1;
                    mask[y * CHUNK + x] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = z;
            greedy_2d(mask, CHUNK, WORLD_H, emit_rect, &e);
        }

    /* WEST/EAST: slice x, mask[y][z] */
    for (int f = F_WEST; f <= F_EAST; f++)
        for (int x = 0; x < CHUNK; x++) {
            int any = 0;
            for (int y = 0; y < WORLD_H; y++)
                for (int z = 0; z < CHUNK; z++) {
                    int blk = g_world[y][bz + z][bx + x];
                    int t = 0;
                    if (blk != B_AIR &&
                        block_at(bx + x + FACES[f].dx, y, bz + z) == B_AIR)
                        t = TILE_FOR[blk][f] + 1;
                    mask[y * CHUNK + z] = (BYTE)t;
                    any |= t;
                }
            if (!any) continue;
            e.face = f; e.slice = x;
            greedy_2d(mask, CHUNK, WORLD_H, emit_rect, &e);
        }
}

static int build_chunk(int cx, int cz)
{
    CHUNKMESH *c = &g_chunks[cz][cx];
    int bx = cx * CHUNK, bz = cz * CHUNK;

    mesh_chunk_quads(cx, cz);
    if (!g_nquads) { c->nverts = c->ntris = 0; return 1; }

    int nverts = g_nquads * 4, nidx = g_nquads * 6;
    VTX  *verts = malloc(sizeof(VTX) * nverts);
    WORD *idx   = malloc(sizeof(WORD) * nidx);
    int vi = 0, ii = 0;

    for (int t = 0; t < NTILES; t++) {
        c->ib_start[t] = ii;
        for (int n = 0; n < g_nquads; n++) {
            const QUAD *q = &g_quads[n];
            if (q->tile != t + 1) continue;
            int s = 255 * FACES[q->face].shade / 100;
            DWORD col = D3DCOLOR_XRGB(s, s, s);
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
            idx[ii++] = (WORD)(vi);     idx[ii++] = (WORD)(vi + 1);
            idx[ii++] = (WORD)(vi + 2); idx[ii++] = (WORD)(vi);
            idx[ii++] = (WORD)(vi + 2); idx[ii++] = (WORD)(vi + 3);
            vi += 4;
        }
        c->prims[t] = (ii - c->ib_start[t]) / 3;
    }
    c->nverts = vi;
    c->ntris  = ii / 3;

    c->min[0] = (float)bx; c->min[1] = 0;              c->min[2] = (float)bz;
    c->max[0] = (float)(bx + CHUNK);
    c->max[1] = (float)WORLD_H;
    c->max[2] = (float)(bz + CHUNK);

    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev,
                   vi * sizeof(VTX), D3DUSAGE_WRITEONLY, VTX_FVF,
                   D3DPOOL_MANAGED, &c->vb, NULL)))
        return 0;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(g_dev,
                   ii * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                   D3DPOOL_MANAGED, &c->ib, NULL)))
        return 0;
    void *p;
    IDirect3DVertexBuffer9_Lock(c->vb, 0, vi * sizeof(VTX), &p, 0);
    memcpy(p, verts, vi * sizeof(VTX));
    IDirect3DVertexBuffer9_Unlock(c->vb);
    IDirect3DIndexBuffer9_Lock(c->ib, 0, ii * sizeof(WORD), &p, 0);
    memcpy(p, idx, ii * sizeof(WORD));
    IDirect3DIndexBuffer9_Unlock(c->ib);

    free(verts);
    free(idx);
    g_world_tris += c->ntris;
    return 1;
}

static int build_world_meshes(void)
{
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    for (int cz = 0; cz < WORLD_CZ; cz++)
        for (int cx = 0; cx < WORLD_CX; cx++)
            if (!build_chunk(cx, cz)) return 0;
    QueryPerformanceCounter(&b);
    g_mesh_ms = (int)((b.QuadPart - a.QuadPart) * 1000 / freq.QuadPart);
    return 1;
}

/* ------------------------------------------------------- editing (M4) ---- */

static void rebuild_chunk(int cx, int cz)
{
    if (cx < 0 || cz < 0 || cx >= WORLD_CX || cz >= WORLD_CZ) return;
    CHUNKMESH *c = &g_chunks[cz][cx];
    g_world_tris -= c->ntris;
    if (c->vb) { IDirect3DVertexBuffer9_Release(c->vb); c->vb = NULL; }
    if (c->ib) { IDirect3DIndexBuffer9_Release(c->ib); c->ib = NULL; }
    build_chunk(cx, cz);
}

static void set_block(int x, int y, int z, int b)
{
    if (x < 0 || y < 1 || z < 0 || x >= WX || y >= WORLD_H || z >= WZ)
        return;                     /* y=0 is bedrock */
    g_world[y][z][x] = (BYTE)b;

    LARGE_INTEGER freq, a, e;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    int cx = x / CHUNK, cz = z / CHUNK;
    rebuild_chunk(cx, cz);
    if (x % CHUNK == 0)         rebuild_chunk(cx - 1, cz);
    if (x % CHUNK == CHUNK - 1) rebuild_chunk(cx + 1, cz);
    if (z % CHUNK == 0)         rebuild_chunk(cx, cz - 1);
    if (z % CHUNK == CHUNK - 1) rebuild_chunk(cx, cz + 1);
    QueryPerformanceCounter(&e);
    g_remesh_ms = (int)((e.QuadPart - a.QuadPart) * 1000 / freq.QuadPart);
}

static void cam_dir(float *dx, float *dy, float *dz)
{
    *dx = sinf(g_yaw) * cosf(g_pitch);
    *dy = -sinf(g_pitch);
    *dz = cosf(g_yaw) * cosf(g_pitch);
}

/* Amanatides-Woo voxel traversal from the eye along the view direction.
 * Returns 1 on hit; hit[] = solid cell, prev[] = last empty cell before it. */
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
        if (block_at(cx, cy, cz) != B_AIR) {
            hit[0] = cx; hit[1] = cy; hit[2] = cz;
            return 1;
        }
    }
    return 0;
}

static void edit_break(void)
{
    int hit[3], prev[3];
    if (raycast(hit, prev) && hit[1] > 0 && hit[1] < WORLD_H)
        set_block(hit[0], hit[1], hit[2], B_AIR);
}

static void edit_place(void)
{
    int hit[3], prev[3];
    if (!raycast(hit, prev)) return;
    if (block_at(prev[0], prev[1], prev[2]) != B_AIR) return;
    /* don't build inside the camera */
    int ex = (int)floorf(g_cam_x), ey = (int)floorf(g_cam_y),
        ez = (int)floorf(g_cam_z);
    if (prev[0] == ex && prev[2] == ez &&
        (prev[1] == ey || prev[1] == ey - 1))
        return;
    set_block(prev[0], prev[1], prev[2], g_sel);
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
    int hdr[4] = { 0x31435058 /* "XPC1" */, WX, WZ, WORLD_H };
    fwrite(hdr, sizeof hdr, 1, fp);
    fwrite(g_world, sizeof(BYTE) * WORLD_H * WZ * WX, 1, fp);
    fclose(fp);
}

static int load_world(void)
{
    FILE *fp = fopen(g_world_file, "rb");
    if (!fp) return 0;
    int hdr[4] = {0};
    if (fread(hdr, sizeof hdr, 1, fp) != 1 ||
        hdr[0] != 0x31435058 || hdr[1] != WX || hdr[2] != WZ ||
        hdr[3] != WORLD_H) {
        fclose(fp);
        return 0;
    }
    g_world = calloc(1, sizeof(BYTE) * WORLD_H * WZ * WX);
    size_t ok = fread(g_world, sizeof(BYTE) * WORLD_H * WZ * WX, 1, fp);
    fclose(fp);
    if (ok != 1) { free(g_world); g_world = NULL; return 0; }
    return 1;
}

/* ------------------------------------------------------------- texture --- */

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
        if (y % 16 == 15) { *r -= 45; *g -= 40; *b -= 30; }  /* board seams */
        if ((x + (y / 16) * 23) % 32 == 0) { *r -= 30; *g -= 25; *b -= 20; }
        break;
    default:
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
                       D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &g_tex[t], NULL)))
            return 0;
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DTexture9_LockRect(g_tex[t], 0, &lr, NULL, 0)))
            return 0;
        for (int y = 0; y < TILE; y++) {
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < TILE; x++) {
                int r, g, b;
                tile_pixel(t, x, y, &r, &g, &b);
                row[x] = D3DCOLOR_XRGB(r & 0xFF, g & 0xFF, b & 0xFF);
            }
        }
        IDirect3DTexture9_UnlockRect(g_tex[t], 0);
    }
    return 1;
}

/* --------------------------------------------------------------- input ---- */

static int g_mouse_reset = 1;

static void apply_range(void)
{
    float zf = g_range * 1.4f;
    if (zf < 80) zf = 80;
    D3DMATRIX proj = mat_perspective(1.22f, (float)WIN_W / WIN_H, 0.3f, zf);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, &proj);

    union { float f; DWORD d; } fs = { g_range * 0.55f }, fe = { g_range * 0.98f };
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGSTART, fs.d);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGEND,   fe.d);
}

static void update_camera(float dt, float t)
{
    if (!g_manual && !g_bench) {
        g_yaw = t * 0.15f;          /* slow turntable at spawn */
        g_pitch = 0.15f;
    }

    if (GetFocus() != g_hwnd) { g_mouse_reset = 1; return; }

    float speed = 10.0f * dt;
    float fx = sinf(g_yaw), fz = cosf(g_yaw);
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);

    if (GetAsyncKeyState('W') & 0x8000) { g_manual = 1; g_cam_x += fx * speed; g_cam_z += fz * speed; }
    if (GetAsyncKeyState('S') & 0x8000) { g_manual = 1; g_cam_x -= fx * speed; g_cam_z -= fz * speed; }
    if (GetAsyncKeyState('D') & 0x8000) { g_manual = 1; g_cam_x += rx * speed; g_cam_z += rz * speed; }
    if (GetAsyncKeyState('A') & 0x8000) { g_manual = 1; g_cam_x -= rx * speed; g_cam_z -= rz * speed; }
    if (GetAsyncKeyState('E') & 0x8000) { g_manual = 1; g_cam_y += speed; }
    if (GetAsyncKeyState('Q') & 0x8000) { g_manual = 1; g_cam_y -= speed; }

    /* hold-repeat only; the initial click arrives via WM_L/RBUTTONDOWN */
    g_click_cd -= dt;
    int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if ((lmb || rmb) && g_click_cd <= 0) {
        if (lmb) edit_break(); else edit_place();
        g_click_cd = 0.22f;
    }
    if (GetAsyncKeyState(VK_OEM_4) & 0x8000) {      /* [ : shrink range */
        if (g_range > 32) { g_range -= 1; apply_range(); }
    }
    if (GetAsyncKeyState(VK_OEM_6) & 0x8000) {      /* ] : grow range */
        if (g_range < 192) { g_range += 1; apply_range(); }
    }

    POINT center = { WIN_W / 2, WIN_H / 2 }, p;
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

/* --------------------------------------------------------------- d3d9 ----- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        else if (w >= '1' && w <= '4') g_sel = (int)(w - '0');
        else if (w == VK_F5) save_world();
        return 0;
    case WM_LBUTTONDOWN:            /* edge-triggered here so even sub-frame
                                       synthetic clicks (nircmd) register */
        g_manual = 1; edit_break(); g_click_cd = 0.3f; return 0;
    case WM_RBUTTONDOWN:
        g_manual = 1; edit_place(); g_click_cd = 0.3f; return 0;
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
    g_pp.BackBufferWidth       = WIN_W;
    g_pp.BackBufferHeight      = WIN_H;
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
    if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                       hwnd, vp, &g_pp, &g_dev))) {
        if (!g_hwvp) return 0;
        g_hwvp = 0;                 /* HW T&L claimed but device refused */
        if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
                       D3DDEVTYPE_HAL, hwnd,
                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev)))
            return 0;
    }

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGCOLOR, FOG_COLOR);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    apply_range();

    return make_textures();
}

/* returns tris drawn; also counts chunks drawn via *nchunks */
static int render_frame(int *nchunks)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           FOG_COLOR, 1.0f, 0);
    int tris = 0, drawn = 0;

    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        D3DMATRIX world = mat_identity();
        D3DMATRIX view  = mat_view();
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &world);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &view);

        D3DMATRIX proj;
        IDirect3DDevice9_GetTransform(g_dev, D3DTS_PROJECTION, &proj);
        D3DMATRIX vp = mat_mul(&view, &proj);
        frustum_from(&vp);

        /* visibility pass: range circle, then frustum */
        static CHUNKMESH *vis[WORLD_CX * WORLD_CZ];
        int nvis = 0;
        float margin = CHUNK * 0.71f;   /* half chunk diagonal */
        for (int cz = 0; cz < WORLD_CZ; cz++)
            for (int cx = 0; cx < WORLD_CX; cx++) {
                CHUNKMESH *c = &g_chunks[cz][cx];
                if (!c->ntris) continue;
                float dx = (c->min[0] + CHUNK / 2.0f) - g_cam_x;
                float dz = (c->min[2] + CHUNK / 2.0f) - g_cam_z;
                if (sqrtf(dx * dx + dz * dz) > g_range + margin) continue;
                if (!aabb_visible(c->min, c->max)) continue;
                vis[nvis++] = c;
            }

        IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
        for (int t = 0; t < NTILES; t++) {
            IDirect3DDevice9_SetTexture(g_dev, 0,
                                        (IDirect3DBaseTexture9 *)g_tex[t]);
            for (int i = 0; i < nvis; i++) {
                CHUNKMESH *c = vis[i];
                if (!c->prims[t]) continue;
                IDirect3DDevice9_SetStreamSource(g_dev, 0, c->vb, 0, sizeof(VTX));
                IDirect3DDevice9_SetIndices(g_dev, c->ib);
                IDirect3DDevice9_DrawIndexedPrimitive(g_dev, D3DPT_TRIANGLELIST,
                        0, 0, c->nverts, c->ib_start[t], c->prims[t]);
                tris += c->prims[t];
            }
        }
        drawn = nvis;

        /* wireframe highlight on the targeted block */
        int hit[3], prev[3];
        if (!g_bench && raycast(hit, prev)) {
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
            IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_LINELIST, 12,
                                             ln, sizeof(LVTX));
        }

        /* crosshair (screen-space, no depth, no fog) */
        if (!g_bench) {
            typedef struct { float x, y, z, rhw; DWORD c; } SVTX;
            float cx = WIN_W / 2.0f, cy = WIN_H / 2.0f;
            DWORD cc = D3DCOLOR_ARGB(255, 240, 240, 240);
            SVTX ch[4] = {
                { cx - 9, cy, 0, 1, cc }, { cx + 10, cy, 0, 1, cc },
                { cx, cy - 9, 0, 1, cc }, { cx, cy + 10, 0, 1, cc },
            };
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, FALSE);
            IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
            IDirect3DDevice9_SetFVF(g_dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_LINELIST, 2,
                                             ch, sizeof(SVTX));
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
            IDirect3DDevice9_SetRenderState(g_dev, D3DRS_FOGENABLE, TRUE);
        }

        IDirect3DDevice9_EndScene(g_dev);
    }

    HRESULT hr = IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST &&
        IDirect3DDevice9_TestCooperativeLevel(g_dev) == D3DERR_DEVICENOTRESET)
        IDirect3DDevice9_Reset(g_dev, &g_pp);

    if (nchunks) *nchunks = drawn;
    return tris;
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

/* returns 0 when the whole bench is done */
static int bench_step(double now, int tris, int nchunks)
{
    double el = now - g_bench_t0;
    if (g_bench_warm) {
        if (el >= 1.0) { g_bench_warm = 0; g_bench_t0 = now; g_bench_frames = 0; }
        return 1;
    }
    g_bench_frames++;
    if (el < 8.0) return 1;

    char line[128];
    sprintf(line, "range=%3.0f  avg_fps=%6.1f  tris=%6d  chunks=%3d\r\n",
            g_range, g_bench_frames / el, tris, nchunks);
    strcat(g_bench_log, line);

    if (++g_bench_idx >= NBENCH) {
        FILE *fp = fopen("bench.txt", "wb");
        if (fp) {
            fprintf(fp, "xp-craft M3 bench  %dx%d  vp=%s  world=%dx%d chunks  "
                        "world_tris=%d  mesh_ms=%d\r\n",
                    WIN_W, WIN_H, g_hwvp ? "hardware" : "software",
                    WORLD_CX, WORLD_CZ, g_world_tris, g_mesh_ms);
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
    g_bench = (cmdline && strstr(cmdline, "bench")) ? 1 : 0;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "xpcraft";
    RegisterClassA(&wc);

    RECT r = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("xpcraft", "xp-craft", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, hinst, NULL);
    if (!init_d3d(g_hwnd)) {
        MessageBoxA(NULL, "D3D9 init failed", "xp-craft", MB_ICONERROR);
        return 1;
    }
    world_file_path();
    if (!load_world()) gen_world();
    if (!build_world_meshes()) {
        MessageBoxA(NULL, "mesh build failed", "xp-craft", MB_ICONERROR);
        return 1;
    }

    /* spawn standing at world center */
    g_cam_x = WX / 2.0f;
    g_cam_z = WZ / 2.0f;
    g_cam_y = terrain_h(WX / 2, WZ / 2) + 1.7f;
    g_pitch = 0.15f;

    ShowWindow(g_hwnd, show);

    LARGE_INTEGER freq, start, t0, now, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    t0 = prev_t = start;
    int frames = 0, tris = 0, nchunks = 0;

    if (g_bench) bench_start(0.0);

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev_t.QuadPart) / (double)freq.QuadPart);
        double td = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        prev_t = now;

        if (g_bench) {
            g_yaw = (float)(td * (2.0 * 3.14159265 / 8.0));  /* full pan / 8s */
            g_pitch = 0.15f;
        } else {
            update_camera(dt, (float)td);
        }

        tris = render_frame(&nchunks);
        frames++;

        if (g_bench && !bench_step(td, tris, nchunks)) break;

        double sec = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (sec >= 1.0) {
            char title[160];
            sprintf(title, "xp-craft - %.0f FPS - r=%.0f - %d tris, %d chunks"
                           " - %s VP - place: %s - remesh %d ms",
                    frames / sec, g_range, tris, nchunks,
                    g_hwvp ? "HW" : "SW", BLOCK_NAME[g_sel], g_remesh_ms);
            SetWindowTextA(g_hwnd, title);
            frames = 0;
            t0 = now;
        }
    }

    if (!g_bench) save_world();

    for (int cz = 0; cz < WORLD_CZ; cz++)
        for (int cx = 0; cx < WORLD_CX; cx++) {
            if (g_chunks[cz][cx].vb) IDirect3DVertexBuffer9_Release(g_chunks[cz][cx].vb);
            if (g_chunks[cz][cx].ib) IDirect3DIndexBuffer9_Release(g_chunks[cz][cx].ib);
        }
    for (int t = 0; t < NTILES; t++)
        if (g_tex[t]) IDirect3DTexture9_Release(g_tex[t]);
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    free(g_world);
    return 0;
}
