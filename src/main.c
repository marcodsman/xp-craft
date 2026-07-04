/* xp-craft milestone 2 — one chunk (16x16x16), naive meshing, texture atlas.
 *
 * Blocks: air/grass/dirt/stone. A face is emitted only when the neighbor is
 * air; the mesh goes into a static write-only vertex buffer built once at
 * startup. Atlas is 4 procedural 64x64 tiles (grass-top, grass-side, dirt,
 * stone) in a 256x64 strip — still zero asset files to deploy.
 *
 * Camera auto-orbits the chunk (so remote screenshots sweep all angles);
 * first WASD/E/Q press switches to manual fly + mouse-look. ESC quits.
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

#define CHUNK 16
#define TILE  64                    /* atlas tile size in pixels */
#define NTILES 4
#define USTEP (1.0f / NTILES)

enum { B_AIR, B_GRASS, B_DIRT, B_STONE };
enum { F_TOP, F_BOTTOM, F_NORTH, F_SOUTH, F_WEST, F_EAST };
enum { T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE };

typedef struct { float x, y, z; DWORD color; float u, v; } VTX;
#define VTX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3D9             *g_d3d;
static IDirect3DDevice9       *g_dev;
static IDirect3DTexture9      *g_tex;
static IDirect3DVertexBuffer9 *g_vb;
static D3DPRESENT_PARAMETERS   g_pp;
static int  g_running = 1;
static int  g_ntris;
static int  g_mesh_ms;
static HWND g_hwnd;

static BYTE g_blocks[CHUNK][CHUNK][CHUNK];   /* [y][z][x] */

/* fly camera (orbit until first movement key) */
static float g_cam_x, g_cam_y, g_cam_z;
static float g_yaw, g_pitch;
static int   g_manual;

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

/* --------------------------------------------------------------- chunk ---- */

static int block_at(int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= CHUNK || y >= CHUNK || z >= CHUNK)
        return B_AIR;
    return g_blocks[y][z][x];
}

static void gen_terrain(void)
{
    for (int z = 0; z < CHUNK; z++)
        for (int x = 0; x < CHUNK; x++) {
            int h = 6 + (int)(3.5f * sinf(x * 0.35f) * cosf(z * 0.28f)
                            + 2.0f * sinf((x + z) * 0.2f));
            if (h < 1) h = 1;
            if (h > 14) h = 14;
            for (int y = 0; y < h; y++) {
                if      (y == h - 1) g_blocks[y][z][x] = B_GRASS;
                else if (y >= h - 4) g_blocks[y][z][x] = B_DIRT;
                else                 g_blocks[y][z][x] = B_STONE;
            }
        }
}

/* ------------------------------------------------------------- meshing --- */
/* Per-face data. Corner order (a,b,c,d) keeps the winding proven in M1:
 * a=uv(0,0) b=uv(1,0) c=uv(1,1) d=uv(0,1); triangles abc + acd, CULL_CW. */

static const struct {
    int dx, dy, dz;          /* neighbor offset */
    float c[4][3];           /* corner offsets a,b,c,d */
    int shade;               /* Minecraft-ish face light, percent */
} FACES[6] = {
    [F_TOP]    = { 0, 1, 0, {{0,1,0},{1,1,0},{1,1,1},{0,1,1}}, 100 },
    [F_BOTTOM] = { 0,-1, 0, {{0,0,1},{1,0,1},{1,0,0},{0,0,0}},  50 },
    [F_NORTH]  = { 0, 0,-1, {{1,1,0},{0,1,0},{0,0,0},{1,0,0}},  80 },
    [F_SOUTH]  = { 0, 0, 1, {{0,1,1},{1,1,1},{1,0,1},{0,0,1}},  80 },
    [F_WEST]   = {-1, 0, 0, {{0,1,0},{0,1,1},{0,0,1},{0,0,0}},  60 },
    [F_EAST]   = { 1, 0, 0, {{1,1,1},{1,1,0},{1,0,0},{1,0,1}},  60 },
};

static const BYTE TILE_FOR[4][6] = {   /* [block][face] -> atlas tile */
    [B_GRASS] = { T_GRASS_TOP, T_DIRT, T_GRASS_SIDE, T_GRASS_SIDE,
                  T_GRASS_SIDE, T_GRASS_SIDE },
    [B_DIRT]  = { T_DIRT, T_DIRT, T_DIRT, T_DIRT, T_DIRT, T_DIRT },
    [B_STONE] = { T_STONE, T_STONE, T_STONE, T_STONE, T_STONE, T_STONE },
};

static VTX *emit_face(VTX *v, int x, int y, int z, int f, int tile)
{
    int s = 255 * FACES[f].shade / 100;
    DWORD col = D3DCOLOR_XRGB(s, s, s);
    float u0 = tile * USTEP, u1 = u0 + USTEP;
    VTX q[4];
    for (int i = 0; i < 4; i++) {
        q[i].x = x + FACES[f].c[i][0];
        q[i].y = y + FACES[f].c[i][1];
        q[i].z = z + FACES[f].c[i][2];
        q[i].color = col;
    }
    q[0].u = u0; q[0].v = 0;
    q[1].u = u1; q[1].v = 0;
    q[2].u = u1; q[2].v = 1;
    q[3].u = u0; q[3].v = 1;
    *v++ = q[0]; *v++ = q[1]; *v++ = q[2];
    *v++ = q[0]; *v++ = q[2]; *v++ = q[3];
    return v;
}

static int build_mesh(void)
{
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);

    /* worst case: every block, all 6 faces */
    VTX *buf = malloc(sizeof(VTX) * CHUNK * CHUNK * CHUNK * 36);
    if (!buf) return 0;
    VTX *v = buf;

    for (int y = 0; y < CHUNK; y++)
        for (int z = 0; z < CHUNK; z++)
            for (int x = 0; x < CHUNK; x++) {
                int blk = g_blocks[y][z][x];
                if (blk == B_AIR) continue;
                for (int f = 0; f < 6; f++)
                    if (block_at(x + FACES[f].dx, y + FACES[f].dy,
                                 z + FACES[f].dz) == B_AIR)
                        v = emit_face(v, x, y, z, f, TILE_FOR[blk][f]);
            }

    int nverts = (int)(v - buf);
    g_ntris = nverts / 3;

    UINT bytes = nverts * sizeof(VTX);
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(g_dev, bytes,
                   D3DUSAGE_WRITEONLY, VTX_FVF, D3DPOOL_MANAGED, &g_vb, NULL))) {
        free(buf);
        return 0;
    }
    void *dst;
    if (FAILED(IDirect3DVertexBuffer9_Lock(g_vb, 0, bytes, &dst, 0))) {
        free(buf);
        return 0;
    }
    memcpy(dst, buf, bytes);
    IDirect3DVertexBuffer9_Unlock(g_vb);
    free(buf);

    QueryPerformanceCounter(&b);
    g_mesh_ms = (int)((b.QuadPart - a.QuadPart) * 1000 / freq.QuadPart);
    return 1;
}

/* ------------------------------------------------------------- texture --- */
/* 256x64 atlas, 4 procedural tiles: grass-top, grass-side, dirt, stone. */

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
        if (y < 8) {                       /* green band along the top */
            int gn = (int)(rng() % 50);
            *r = 60 + gn / 3; *g = 140 + gn; *b = 50 + gn / 3;
        }
        break;
    case T_DIRT:
        n = (int)(rng() % 40);
        *r = 134 + n - 20; *g = 96 + n - 20; *b = 67 + n - 20;
        break;
    default:                               /* T_STONE */
        n = (int)(rng() % 35);
        *r = *g = *b = 110 + n - 17;
        break;
    }
    if (x == 0 || y == 0) { *r -= 25; *g -= 25; *b -= 25; }   /* block edge */
}

static int make_atlas(void)
{
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, TILE * NTILES, TILE, 1, 0,
                                              D3DFMT_X8R8G8B8, D3DPOOL_MANAGED,
                                              &g_tex, NULL)))
        return 0;

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_tex, 0, &lr, NULL, 0))) return 0;
    for (int y = 0; y < TILE; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int t = 0; t < NTILES; t++)
            for (int x = 0; x < TILE; x++) {
                int r, g, b;
                tile_pixel(t, x, y, &r, &g, &b);
                row[t * TILE + x] = D3DCOLOR_XRGB(r & 0xFF, g & 0xFF, b & 0xFF);
            }
    }
    IDirect3DTexture9_UnlockRect(g_tex, 0);
    return 1;
}

/* --------------------------------------------------------------- input ---- */

static int g_mouse_reset = 1;   /* skip the delta on the first focused frame */

static void update_camera(float dt, float t)
{
    float cx = CHUNK / 2.0f, cy = CHUNK / 2.0f, cz = CHUNK / 2.0f;

    if (!g_manual) {
        /* slow orbit around the chunk, looking at its center */
        float a = t * 0.25f, r = 22.0f, h = 12.0f;
        g_cam_x = cx + sinf(a) * r;
        g_cam_z = cz + cosf(a) * r;
        g_cam_y = cy + h;
        g_yaw   = a + 3.14159265f;
        g_pitch = atan2f(h, r);
    }

    if (GetFocus() != g_hwnd) { g_mouse_reset = 1; return; }

    float speed = 8.0f * dt;
    float fx = sinf(g_yaw), fz = cosf(g_yaw);
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);

    if (GetAsyncKeyState('W') & 0x8000) { g_manual = 1; g_cam_x += fx * speed; g_cam_z += fz * speed; }
    if (GetAsyncKeyState('S') & 0x8000) { g_manual = 1; g_cam_x -= fx * speed; g_cam_z -= fz * speed; }
    if (GetAsyncKeyState('D') & 0x8000) { g_manual = 1; g_cam_x += rx * speed; g_cam_z += rz * speed; }
    if (GetAsyncKeyState('A') & 0x8000) { g_manual = 1; g_cam_x -= rx * speed; g_cam_z -= rz * speed; }
    if (GetAsyncKeyState('E') & 0x8000) { g_manual = 1; g_cam_y += speed; }
    if (GetAsyncKeyState('Q') & 0x8000) { g_manual = 1; g_cam_y -= speed; }

    POINT center = { WIN_W / 2, WIN_H / 2 }, p;
    ClientToScreen(g_hwnd, &center);
    GetCursorPos(&p);
    if (g_mouse_reset) {          /* stale cursor position — don't apply it */
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
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
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
    g_pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE; /* no vsync: measure */

    if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                       hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                       &g_pp, &g_dev)))
        return 0;

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    D3DMATRIX proj = mat_perspective(1.22f /* ~70 deg */,
                                     (float)WIN_W / WIN_H, 0.1f, 100.0f);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, &proj);
    return make_atlas();
}

static void render_frame(void)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(0x87, 0xCE, 0xEB), 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        D3DMATRIX world = mat_identity();
        D3DMATRIX view  = mat_view();
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &world);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &view);
        IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)g_tex);
        IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
        IDirect3DDevice9_SetStreamSource(g_dev, 0, g_vb, 0, sizeof(VTX));
        IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLELIST, 0, g_ntris);
        IDirect3DDevice9_EndScene(g_dev);
    }

    HRESULT hr = IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST &&
        IDirect3DDevice9_TestCooperativeLevel(g_dev) == D3DERR_DEVICENOTRESET)
        IDirect3DDevice9_Reset(g_dev, &g_pp);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev; (void)cmdline;

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
    gen_terrain();
    if (!build_mesh()) {
        MessageBoxA(NULL, "mesh build failed", "xp-craft", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, show);

    LARGE_INTEGER freq, start, t0, now, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    t0 = prev_t = start;
    int frames = 0;

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev_t.QuadPart) / (double)freq.QuadPart);
        float t  = (float)((double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart);
        prev_t = now;

        update_camera(dt, t);
        render_frame();
        frames++;

        double sec = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (sec >= 1.0) {
            char title[96];
            sprintf(title, "xp-craft - %.0f FPS - %d tris (mesh %d ms)",
                    frames / sec, g_ntris, g_mesh_ms);
            SetWindowTextA(g_hwnd, title);
            frames = 0;
            t0 = now;
        }
    }

    if (g_vb)  IDirect3DVertexBuffer9_Release(g_vb);
    if (g_tex) IDirect3DTexture9_Release(g_tex);
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    return 0;
}
