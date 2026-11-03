/*
 * igs-loader-gui on Windows: a Win32 window with a WGL (OpenGL 3) context,
 * Dear ImGui's Win32 and OpenGL3 backends (XInput gamepads included).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include "gui.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static HDC dc;
static HGLRC rc;
static int width, height;

static bool gl_create(HWND w)
{
    PIXELFORMATDESCRIPTOR pfd = { 0 };
    typedef HGLRC(WINAPI * create_attribs)(HDC, HGLRC, const int *);
    typedef BOOL(WINAPI * swap_interval)(int);
    static const int attribs[] = { 0x2091, 3, 0x2092, 0, 0 };     /* WGL_CONTEXT_MAJOR/MINOR_VERSION_ARB: 3.0 */
    create_attribs ca;
    swap_interval si;
    HGLRC tmp, ctx;
    int pf;

    dc = GetDC(w);
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    if (!(pf = ChoosePixelFormat(dc, &pfd)) || !SetPixelFormat(dc, pf, &pfd))
        return false;
    if (!(tmp = wglCreateContext(dc)) || !wglMakeCurrent(dc, tmp))
        return false;
    rc = tmp;
    ca = (create_attribs)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (ca && (ctx = ca(dc, 0, attribs))) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(tmp);
        rc = ctx;
        wglMakeCurrent(dc, rc);
    }
    if ((si = (swap_interval)(void *)wglGetProcAddress("wglSwapIntervalEXT")))
        si(1);
    return true;
}

unsigned gfx_texture(const unsigned char *rgba, int w, int h, unsigned old)
{
    GLuint t = old;

    if (!t)
        glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

void gfx_free(unsigned tex)
{
    GLuint t = tex;

    if (t)
        glDeleteTextures(1, &t);
}

static LRESULT WINAPI wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(w, msg, wp, lp))
        return 1;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            width = LOWORD(lp);
            height = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        if (app_close_request())
            DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int main(int argc, char **argv)
{
    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, wnd_proc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL,
                       L"igs-loader-gui", NULL };
    float scale;
    HWND w;
    bool done = false;

    app_init(argc, argv);
    ImGui_ImplWin32_EnableDpiAwareness();
    scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    w = CreateWindowW(wc.lpszClassName, L"IGS Loader", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                      (int)(1360 * scale), (int)(860 * scale), NULL, NULL, wc.hInstance, NULL);
    if (!w || !gl_create(w)) {
        MessageBoxA(NULL, "No OpenGL 3 context", "igs-loader-gui", MB_ICONERROR);
        return 1;
    }
    /* IGS_BACKGROUND (test runs): no focus, at the bottom */
    if (app_background()) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(w, SW_SHOWDEFAULT);
    }
    UpdateWindow(w);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui::GetStyle().FontSizeBase = app_font_size();
    if (sys_exists(app_font_file()))
        io.Fonts->AddFontFromFileTTF(app_font_file().c_str());
    ImGui_ImplWin32_InitForOpenGL(w);
    ImGui_ImplOpenGL3_Init();

    while (!done) {
        MSG msg;

        /* idle: up to 50 ms between frames, so the logs still scroll */
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (app_quit()) {
            DestroyWindow(w);
            continue;
        }
        if (IsIconic(w))
            continue;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app_frame();
        ImGui::Render();
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.08f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::string shot = app_shot();
        if (!shot.empty() && width > 0 && height > 0) {
            std::vector<unsigned char> px((size_t)width * height * 4);

            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            app_write_bmp(shot, px.data(), width, height);
        }
        SwapBuffers(dc);
    }
    app_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
