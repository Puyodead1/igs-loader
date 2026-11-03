/*
 * igs-loader-gui on Linux: an SDL2 window with an OpenGL 3 context, Dear
 * ImGui's SDL2 and OpenGL3 backends (SDL game controllers included).
 */
#include <SDL.h>
#include <SDL_opengl.h>

#include "gui.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

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

int main(int argc, char **argv)
{
    SDL_Window *w;
    SDL_GLContext gl;
    float scale;
    bool done = false;

    app_init(argc, argv);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "igs-loader-gui: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    w = SDL_CreateWindow("IGS Loader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1360 * scale),
                         (int)(860 * scale), SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w || !(gl = SDL_GL_CreateContext(w))) {
        fprintf(stderr, "igs-loader-gui: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(w, gl);
    SDL_GL_SetSwapInterval(1);

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
    ImGui_ImplSDL2_InitForOpenGL(w, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    while (!done) {
        SDL_Event ev;

        /* idle: up to 50 ms between frames, so the logs still scroll */
        if (SDL_WaitEventTimeout(&ev, 50))
            do {
                ImGui_ImplSDL2_ProcessEvent(&ev);
                if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE
                                            && ev.window.windowID == SDL_GetWindowID(w)))
                    done = app_close_request();
            } while (SDL_PollEvent(&ev));
        if (done || app_quit())
            break;
        if (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED)
            continue;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        app_frame();
        ImGui::Render();
        int fw, fh;
        SDL_GL_GetDrawableSize(w, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.08f, 0.08f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::string shot = app_shot();
        if (!shot.empty() && fw > 0 && fh > 0) {
            std::vector<unsigned char> px((size_t)fw * fh * 4);

            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            app_write_bmp(shot, px.data(), fw, fh);
        }
        SDL_GL_SwapWindow(w);
    }
    app_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
