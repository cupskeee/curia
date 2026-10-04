// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>

#include "ui/render_backend.h"

namespace curia::ui {

namespace {

class SdlRendererBackend final : public IRenderBackend {
public:
    ~SdlRendererBackend() override { shutdown(); }

    bool init(SDL_Window* window) override {
        renderer_ = SDL_CreateRenderer(window, nullptr);
        if (renderer_ == nullptr) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer_, 1);
        if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer_)) {
            std::fprintf(stderr, "ImGui_ImplSDL3_InitForSDLRenderer failed\n");
            shutdown();
            return false;
        }
        if (!ImGui_ImplSDLRenderer3_Init(renderer_)) {
            std::fprintf(stderr, "ImGui_ImplSDLRenderer3_Init failed\n");
            ImGui_ImplSDL3_Shutdown();
            shutdown();
            return false;
        }
        initialised_ = true;
        return true;
    }

    void processEvent(const SDL_Event& event) override { ImGui_ImplSDL3_ProcessEvent(&event); }

    void beginFrame() override {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void endFrame() override {
        ImGui::Render();
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer_, 24, 20, 16, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

    void shutdown() override {
        if (initialised_) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            initialised_ = false;
        }
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
    }

    std::string rendererName() const override {
        const char* name = renderer_ != nullptr ? SDL_GetRendererName(renderer_) : nullptr;
        return name != nullptr ? name : "none";
    }

private:
    SDL_Renderer* renderer_ = nullptr;
    bool initialised_ = false;
};

}  // namespace

std::unique_ptr<IRenderBackend> createSdlRendererBackend() {
    return std::make_unique<SdlRendererBackend>();
}

}  // namespace curia::ui
