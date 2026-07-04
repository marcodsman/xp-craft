/* xp-craft milestone 0 — walking skeleton.
 *
 * Window + D3D9 device + sky-blue clear + one triangle + FPS in the title bar.
 * Proves the whole pipeline end to end: cross-compile on Linux, deploy over the
 * share, launch via xprun, read the FPS off an xpshot screenshot.
 *
 * D3D9 usage mirrors xp-gpu-trace/tri_d3d9.c, which is proven on this box:
 * fixed-function, software vertex processing, no shaders.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define WIN_W 800
#define WIN_H 600

typedef struct { float x, y, z, rhw; DWORD color; } VTX;
#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static IDirect3D9       *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DPRESENT_PARAMETERS g_pp;
static int g_running = 1;

static const VTX g_tri[3] = {
    { WIN_W * 0.50f, WIN_H * 0.25f, 0.0f, 1.0f, 0xFF55CC55 }, /* grass green  */
    { WIN_W * 0.70f, WIN_H * 0.65f, 0.0f, 1.0f, 0xFF8B5A2B }, /* dirt brown   */
    { WIN_W * 0.30f, WIN_H * 0.65f, 0.0f, 1.0f, 0xFF8B5A2B },
};

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static int init_d3d(HWND hwnd)
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return 0;

    ZeroMemory(&g_pp, sizeof g_pp);
    g_pp.Windowed          = TRUE;
    g_pp.SwapEffect        = D3DSWAPEFFECT_DISCARD;
    g_pp.hDeviceWindow     = hwnd;
    g_pp.BackBufferWidth   = WIN_W;
    g_pp.BackBufferHeight  = WIN_H;
    /* D3DFMT_UNKNOWN = match the desktop mode; revisit 16-bit in milestone 1 */
    g_pp.BackBufferFormat  = D3DFMT_UNKNOWN;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; /* no vsync: measure */

    HRESULT hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                         hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                         &g_pp, &g_dev);
    if (FAILED(hr)) return 0;

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    return 1;
}

static void render_frame(void)
{
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(0x87, 0xCE, 0xEB) /* sky blue */, 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) {
        IDirect3DDevice9_SetFVF(g_dev, VTX_FVF);
        IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 1,
                                         g_tri, sizeof(VTX));
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

    /* Client area exactly WIN_W x WIN_H */
    RECT r = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("xpcraft", "xp-craft", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, hinst, NULL);
    if (!init_d3d(hwnd)) {
        MessageBoxA(NULL, "D3D9 init failed", "xp-craft", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, show);

    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int frames = 0;

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        render_frame();
        frames++;

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (dt >= 1.0) {
            char title[64];
            sprintf(title, "xp-craft - %.0f FPS", frames / dt);
            SetWindowTextA(hwnd, title);
            frames = 0;
            t0 = now;
        }
    }

    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    return 0;
}
