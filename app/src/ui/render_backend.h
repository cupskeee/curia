// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace curia::ui {

// The seam between the UI code and the renderer (decision D1). Today the only implementation is
// SDL_Renderer + imgui_impl_sdlrenderer3; SDL_GPU can replace it once SDL supports transparent
// windows there (SDL issue #15181). Keep this interface to the ImGui backend calls only.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool init(SDL_Window* window) = 0;
    virtual void processEvent(const SDL_Event& event) = 0;
    virtual void beginFrame() = 0;  // starts a Dear ImGui frame
    virtual void endFrame() = 0;    // renders the ImGui draw data and presents
    // Colour the frame is cleared to before the ImGui draw data; alpha 0 gives a transparent
    // window.
    virtual void setClearColor(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) = 0;
    virtual void shutdown() = 0;
    virtual std::string rendererName() const = 0;
};

std::unique_ptr<IRenderBackend> createSdlRendererBackend();

}  // namespace curia::ui
