/* xp-craft milestone 1 — textured spinning cube + fly camera + depth buffer.
 *
 * First real 3D: fixed-function T&L (SetTransform), D16 depth, point-sampled
 * procedural texture, Minecraft-style per-face shading baked into vertex color.
 * The cube autorotates so an xpshot screenshot proves rendering without input;
 * at the box: WASD + E/Q fly, mouse-look while the window has focus, ESC quits.
 *
 * No D3DX (mingw-w64 doesn't ship it) — small row-vector mat4 helpers instead.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>

#define WIN_W 800
#define WIN_H 600
#define TEX_SIZE 64

typedef struct { float x, y, z; DWORD color; float u, v; } VTX;
#define VTX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3D9        *g_d3d;
static IDirect3DDevice9  *g_dev;
static IDirect3DTexture9 *g_tex;
static D3DPRESENT_PARAMETERS g_pp;
static int  g_running = 1;
static HWND g_hwnd;

/* fly camera — spawn above and behind, looking down at the cube so a single
 * screenshot exercises top face, two sides, shading, and culling at once */
static float g_cam_x, g_cam_y = 2.2f, g_cam_z = -2.6f;
static float g_yaw, g_pitch = 0.7f;

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

/* left-handed perspective, like D3DXMatrixPerspectiveFovLH */
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

/* world -> view: undo camera position, then yaw, then pitch */
static D3DMATRIX mat_view(void)
{
    D3DMATRIX t  = mat_translate(-g_cam_x, -g_cam_y, -g_cam_z);
    D3DMATRIX ry = mat_rot_y(-g_yaw);
    D3DMATRIX rx = mat_rot_x(-g_pitch);
    D3DMATRIX m  = mat_mul(&t, &ry);
    return mat_mul(&m, &rx);
}

/* ---------------------------------------------------------------- cube ---- */
/* Minecraft-ish face shading baked into diffuse: top 100%, N/S 80%, E/W 60%,
 * bottom 50%. Texture modulates against it (stage-0 default MODULATE). */

static DWORD shade(int pct)
{
    int v = 255 * pct / 100;
    return D3DCOLOR_XRGB(v, v, v);
}

#define FACE(ax,ay,az, bx,by,bz, cx,cy,cz, dx,dy,dz, col) \
    { ax,ay,az, col, 0,0 }, { bx,by,bz, col, 1,0 }, { cx,cy,cz, col, 1,1 }, \
    { ax,ay,az, col, 0,0 }, { cx,cy,cz, col, 1,1 }, { dx,dy,dz, col, 0,1 }

static VTX g_cube[36];

static void build_cube(void)
{
    const VTX cube[36] = {
        /* +Y top    */ FACE(-.5f, .5f,-.5f,  .5f, .5f,-.5f,  .5f, .5f, .5f, -.5f, .5f, .5f, shade(100)),
        /* -Y bottom */ FACE(-.5f,-.5f, .5f,  .5f,-.5f, .5f,  .5f,-.5f,-.5f, -.5f,-.5f,-.5f, shade(50)),
        /* -Z north  */ FACE( .5f, .5f,-.5f, -.5f, .5f,-.5f, -.5f,-.5f,-.5f,  .5f,-.5f,-.5f, shade(80)),
        /* +Z south  */ FACE(-.5f, .5f, .5f,  .5f, .5f, .5f,  .5f,-.5f, .5f, -.5f,-.5f, .5f, shade(80)),
        /* -X west   */ FACE(-.5f, .5f,-.5f, -.5f, .5f, .5f, -.5f,-.5f, .5f, -.5f,-.5f,-.5f, shade(60)),
        /* +X east   */ FACE( .5f, .5f, .5f,  .5f, .5f,-.5f,  .5f,-.5f,-.5f,  .5f,-.5f, .5f, shade(60)),
    };
    memcpy(g_cube, cube, sizeof cube);
}

/* ------------------------------------------------------------- texture --- */
/* Procedural dirt block with a mossy top edge — no asset files to deploy. */

static unsigned g_rng = 0x12345678u;
static unsigned rng(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 16; }

static int make_texture(void)
{
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, TEX_SIZE, TEX_SIZE, 1, 0,
                                              D3DFMT_X8R8G8B8, D3DPOOL_MANAGED,
                                              &g_tex, NULL)))
        return 0;

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_tex, 0, &lr, NULL, 0))) return 0;
    for (int y = 0; y < TEX_SIZE; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < TEX_SIZE; x++) {
            int n = (int)(rng() % 40);                 /* dirt speckle */
            int rr = 134 + n - 20, gg = 96 + n - 20, bb = 67 + n - 20;
            if (y < 8) {                               /* mossy top band */
                int gn = (int)(rng() % 50);
                rr = 60 + gn / 3; gg = 140 + gn; bb = 50 + gn / 3;
            }
            if (x == 0 || y == 0) { rr -= 25; gg -= 25; bb -= 25; } /* block edge */
            row[x] = D3DCOLOR_XRGB(rr & 0xFF, gg & 0xFF, bb & 0xFF);
        }
    }
    IDirect3DTexture9_UnlockRect(g_tex, 0);
    return 1;
}

/* --------------------------------------------------------------- input ---- */

static int g_mouse_reset = 1;   /* skip the delta on the first focused frame */

static void update_camera(float dt)
{
    if (GetFocus() != g_hwnd) { g_mouse_reset = 1; return; }

    float speed = 5.0f * dt;
    float fx = sinf(g_yaw), fz = cosf(g_yaw);   /* forward (horizontal) */
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);  /* right */

    if (GetAsyncKeyState('W') & 0x8000) { g_cam_x += fx * speed; g_cam_z += fz * speed; }
    if (GetAsyncKeyState('S') & 0x8000) { g_cam_x -= fx * speed; g_cam_z -= fz * speed; }
    if (GetAsyncKeyState('D') & 0x8000) { g_cam_x += rx * speed; g_cam_z += rz * speed; }
    if (GetAsyncKeyState('A') & 0x8000) { g_cam_x -= rx * speed; g_cam_z -= rz * speed; }
    if (GetAsyncKeyState('E') & 0x8000) g_cam_y += speed;
    if (GetAsyncKeyState('Q') & 0x8000) g_cam_y -= speed;

    /* mouse-look: delta from window center, then recenter the cursor */
    POINT center = { WIN_W / 2, WIN_H / 2 }, p;
    ClientToScreen(g_hwnd, &center);
    GetCursorPos(&p);
    if (g_mouse_reset) {          /* stale cursor position — don't apply it */
        g_mouse_reset = 0;
        SetCursorPos(center.x, center.y);
        return;
    }
    g_yaw   += (p.x - center.x) * 0.003f;
    g_pitch += (p.y - center.y) * 0.003f;
    if (g_pitch >  1.55f) g_pitch =  1.55f;
    if (g_pitch < -1.55f) g_pitch = -1.55f;
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
    /* point sampling = the Minecraft look, and the cheapest filter there is */
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    D3DMATRIX proj = mat_perspective(1.22f /* ~70 deg */,
                                     (float)WIN_W / WIN_H, 0.1f, 100.0f);
    IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, &proj);
    return make_texture();
}

static void render_frame(float t)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(0x87, 0xCE, 0xEB), 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        D3DMATRIX spin = mat_rot_y(t * 0.7f);
        D3DMATRIX view = mat_view();
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &spin);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &view);
        IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)g_tex);
        IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
        IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 12,
                                         g_cube, sizeof(VTX));
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
    build_cube();
    ShowWindow(g_hwnd, show);

    LARGE_INTEGER freq, start, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    t0 = start;
    LARGE_INTEGER prev_t = start;
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

        update_camera(dt);
        render_frame(t);
        frames++;

        double sec = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (sec >= 1.0) {
            char title[64];
            sprintf(title, "xp-craft - %.0f FPS", frames / sec);
            SetWindowTextA(g_hwnd, title);
            frames = 0;
            t0 = now;
        }
    }

    if (g_tex) IDirect3DTexture9_Release(g_tex);
    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    return 0;
}
