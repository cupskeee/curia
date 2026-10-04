// SPDX-License-Identifier: MIT
// macOS overlay probe for milestone M2 (docs/spikes/m2-checklist.md). Throwaway spike code: it
// measures how an SDL3 + Dear ImGui overlay behaves over CK3 on a Mac. It writes only its own log
// file, reads nothing from the game, and never shows a permission prompt itself.
//
// Timeline of a stage: launch, a start delay (to bring CK3 to the front; the window stays hidden),
// the "go" sound (the stage clock starts), the stage runs, then it quits by itself.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "probe_combos.h"
#include "probe_log.h"
#include "probe_mac.h"
#include "probe_options.h"
#include "ui/render_backend.h"

namespace {

using namespace curia::probe;
namespace mac = curia::probe::mac;
namespace fs = std::filesystem;

constexpr int kPanelWidth = 560;
constexpr int kPanelHeight = 340;
constexpr int kPanelMargin = 40;
constexpr int kFramesAfterEvent = 2;
constexpr double kClickThroughSeconds = 10.0;
constexpr double kPostActionLogSeconds = 0.5;
constexpr double kSettledLogSeconds = 1.5;
constexpr double kStateLogSeconds = 2.0;
constexpr double kCpuLogSeconds = 10.0;

// Event types registered at startup. The timer, the Carbon hot-key handler and the tray callbacks
// only push these; everything else happens on the main thread.
struct CustomEvents {
    Uint32 tick = 0;
    Uint32 hotkey = 0;
    Uint32 tray = 0;
};
CustomEvents gEvents;

Uint32 SDLCALL tickCallback(void* /*userdata*/, SDL_TimerID /*id*/, Uint32 interval) {
    SDL_Event event;
    SDL_zero(event);
    event.type = gEvents.tick;
    SDL_PushEvent(&event);
    return interval;
}

void pushCustom(Uint32 type, int code) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.user.code = code;
    SDL_PushEvent(&event);
}

void SDLCALL trayEntryCallback(void* userdata, SDL_TrayEntry* /*entry*/) {
    pushCustom(gEvents.tray, static_cast<int>(reinterpret_cast<std::intptr_t>(userdata)));
}

std::string timestampForFileName() {
    const std::string text = ProbeLog::formatTimestamp(std::chrono::system_clock::now(), false);
    std::string out;
    for (const char c : text.substr(0, 19)) {
        if (c == '-' || c == ':') {
            continue;
        }
        out += (c == ' ') ? '-' : c;
    }
    return out;
}

double cpuMilliseconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto toMs = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_usec) / 1000.0;
    };
    return toMs(usage.ru_utime) + toMs(usage.ru_stime);
}

const char* windowEventName(Uint32 type) {
    switch (type) {
    case SDL_EVENT_WINDOW_SHOWN:
        return "window_shown";
    case SDL_EVENT_WINDOW_HIDDEN:
        return "window_hidden";
    case SDL_EVENT_WINDOW_EXPOSED:
        return "window_exposed";
    case SDL_EVENT_WINDOW_MOVED:
        return "window_moved";
    case SDL_EVENT_WINDOW_RESIZED:
        return "window_resized";
    case SDL_EVENT_WINDOW_OCCLUDED:
        return "window_occluded";
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        return "window_mouse_enter";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        return "window_mouse_leave";
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "window_focus_gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "window_focus_lost";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        return "window_display_changed";
    default:
        return nullptr;
    }
}

class ProbeRun {
public:
    explicit ProbeRun(ProbeOptions options) : opt_(std::move(options)) {}
    int run();

private:
    bool usesPanel() const {
        return opt_.stage != Stage::E1 && opt_.stage != Stage::E2 && opt_.stage != Stage::Detect;
    }
    bool hasWindow() const { return opt_.stage != Stage::Detect; }
    bool drawable() const { return went_ && hasWindow() && (!usesPanel() || panelShown_); }
    bool transparentStage() const {
        return opt_.stage == Stage::E4 || opt_.stage == Stage::Interactive ||
               opt_.stage == Stage::Tray;
    }
    bool interactiveStage() const {
        return opt_.stage == Stage::Interactive || opt_.stage == Stage::Tray;
    }
    bool cycling() const { return opt_.stage == Stage::E3 && opt_.combo == 0; }
    Uint32 tickMs() const { return opt_.stage == Stage::E4 ? 1000U : 250U; }

    bool openLog();
    bool createWindow();
    void* cocoaWindow() const;
    void createTray();
    void go();
    void applyActiveCombo(bool replay, bool cue);
    void showPanel(const char* why);
    void dismissPanel(const char* why);
    void setClickThrough(bool on, const char* why);
    void reactivate(const char* why);
    void handleEvent(const SDL_Event& event);
    void handleTick();
    void handleHotkey(int id);
    void handleTray(int id);
    void logFocus(const char* why);
    void logCpu(const char* why);
    void draw();
    void drawBanner();
    void drawTransparentTest();
    void drawInteractive();

    // Seconds since "go" (negative during the start delay) and since launch.
    double sinceLaunch() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - launch_).count();
    }
    double elapsed() const { return sinceLaunch() - delaySeconds_; }
    void quit(const char* reason) {
        log_.write("quit", fields({kv("reason", reason), kv("t", elapsed(), 2)}));
        running_ = false;
    }

    ProbeOptions opt_;
    ProbeLog log_;
    SDL_Window* window_ = nullptr;
    void* panel_ = nullptr;
    std::unique_ptr<curia::ui::IRenderBackend> backend_;
    SDL_Tray* tray_ = nullptr;
    bool trayCreated_ = false;
    bool running_ = true;
    bool dirty_ = false;
    bool went_ = false;
    int redraw_ = kFramesAfterEvent;
    std::uint64_t frames_ = 0;
    std::chrono::steady_clock::time_point launch_ = std::chrono::steady_clock::now();
    double delaySeconds_ = 0.0;

    int comboIndex_ = 0;  // index into allCombos()
    int lastStep_ = -1;   // E3 cycle step that was applied last
    bool panelShown_ = false;
    bool clickThrough_ = false;
    double clickThroughUntil_ = 0.0;
    double postActionLogAt_ = -1.0;
    const char* postActionLabel_ = "after_action";
    double settledLogAt_ = -1.0;
    double nextStateLog_ = kStateLogSeconds;
    double nextCpuLog_ = kCpuLogSeconds;
    std::string lastFocus_;
    std::string hotkeyStatus_ = "hot keys: not registered";

    // Interactive stage counters (shown in the window and logged).
    char textBuffer_[256] = {};
    int mouseDowns_ = 0;
    int keyDowns_ = 0;
    int textInputs_ = 0;
    std::string lastKeys_;
};

bool ProbeRun::openLog() {
    std::string dir = opt_.resultsDir;
    if (dir.empty()) {
        dir = mac::homeDirectory() + "/curia_m2_results";
    }
    std::error_code error;
    fs::create_directories(dir, error);
    const std::string path =
        dir + "/probe-" + stageName(opt_.stage) + "-" + timestampForFileName() + ".log";
    if (!log_.open(path, opt_.echo)) {
        std::fprintf(stderr, "cannot open log file %s\n", path.c_str());
        return false;
    }
    return true;
}

void* ProbeRun::cocoaWindow() const {
    if (window_ == nullptr) {
        return nullptr;
    }
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                  SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

void ProbeRun::logFocus(const char* why) {
    lastFocus_ = mac::focusSnapshot(panel_);
    std::string combo;
    if (usesPanel()) {
        combo =
            kv("combo",
               static_cast<long long>(allCombos()[static_cast<std::size_t>(comboIndex_)].number));
    }
    log_.write("focus", fields({kv("why", why), kv("t", elapsed(), 2), combo, lastFocus_}));
}

void ProbeRun::logCpu(const char* why) {
    log_.write("cpu",
               fields({kv("why", why), kv("t", elapsed(), 2), kv("cpu_ms", cpuMilliseconds(), 1),
                       kv("frames", static_cast<long long>(frames_))}));
}

// Every combination is applied the same way: hide, set level and behaviour, show. That includes
// the first one, so no combination gets a different first-show path than the others.
void ProbeRun::applyActiveCombo(bool replay, bool cue) {
    const auto& combos = allCombos();
    const Combo& combo = combos[static_cast<std::size_t>(comboIndex_)];
    mac::hidePanel(panel_);
    mac::applyCombo(panel_, combo);
    mac::showPanel(panel_);
    panelShown_ = true;
    log_.write(
        "combo",
        fields({kv("number", static_cast<long long>(combo.number)),
                kv("level", static_cast<long long>(combo.level)), kv("level_name", combo.levelName),
                kv("flag_set", std::string(1, combo.flagSet)), kv("flags", flagNames(combo.flags)),
                kv("replay", replay ? 1LL : 0LL), kv("t", elapsed(), 2)}));
    if (cue && opt_.sound) {
        mac::playSound("Tink");
    }
    logFocus("combo");
    settledLogAt_ = elapsed() + (cycling() ? std::min(kSettledLogSeconds, opt_.stepSeconds * 0.75)
                                           : kSettledLogSeconds);
    dirty_ = true;
}

void ProbeRun::showPanel(const char* why) {
    mac::rememberFrontmost();
    mac::showPanel(panel_);
    panelShown_ = true;
    log_.write("show", kv("why", why));
    logFocus("show");
    dirty_ = true;
}

void ProbeRun::dismissPanel(const char* why) {
    mac::hidePanel(panel_);
    panelShown_ = false;
    log_.write("dismiss", fields({kv("why", why), kv("t", elapsed(), 2)}));
    logFocus("dismiss");
    postActionLogAt_ = elapsed() + kPostActionLogSeconds;
    postActionLabel_ = "after_dismiss";
}

void ProbeRun::reactivate(const char* why) {
    const bool accepted = mac::reactivateRemembered();
    log_.write("reactivate", fields({kv("why", why), kv("accepted", accepted ? 1LL : 0LL),
                                     mac::rememberedLabel(), kv("t", elapsed(), 2)}));
    logFocus("reactivate");
    postActionLogAt_ = elapsed() + kPostActionLogSeconds;
    postActionLabel_ = "after_reactivate";
}

void ProbeRun::setClickThrough(bool on, const char* why) {
    mac::setIgnoresMouse(panel_, on);
    clickThrough_ = on;
    clickThroughUntil_ = on ? elapsed() + kClickThroughSeconds : 0.0;
    log_.write("click_through", fields({kv("on", on ? 1LL : 0LL), kv("why", why)}));
    logFocus("click_through");
    dirty_ = true;
}

bool ProbeRun::createWindow() {
    if (!usesPanel()) {
        // E1 and E2: a plain SDL window created the way the skeleton app creates one, always on
        // top. It is created hidden and shown at "go".
        window_ = SDL_CreateWindow("Curia M2 probe", kPanelWidth, kPanelHeight,
                                   SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_TRANSPARENT |
                                       SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                       SDL_WINDOW_HIDDEN);
        return window_ != nullptr;
    }

    panel_ = mac::createPanel(kPanelWidth, kPanelHeight, transparentStage());
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER, panel_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Curia M2 probe");
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window_ != nullptr;
}

void ProbeRun::createTray() {
    log_.write("tray_create_begin", fields({kv("t", elapsed(), 2), mac::focusSnapshot(panel_)}));
    SDL_Surface* icon = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr) {
        SDL_FillSurfaceRect(icon, nullptr, SDL_MapSurfaceRGBA(icon, 255, 140, 0, 255));
    }
    tray_ = SDL_CreateTray(icon, "Curia M2 probe");
    if (icon != nullptr) {
        SDL_DestroySurface(icon);
    }
    trayCreated_ = true;
    if (tray_ == nullptr) {
        log_.write("tray_create_failed", kv("error", SDL_GetError()));
        return;
    }
    SDL_TrayMenu* menu = SDL_CreateTrayMenu(tray_);
    const char* labels[] = {"Show", "Hide", "Quit"};
    for (int i = 0; i < 3; ++i) {
        SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(menu, -1, labels[i], SDL_TRAYENTRY_BUTTON);
        if (entry != nullptr) {
            SDL_SetTrayEntryCallback(entry, trayEntryCallback,
                                     reinterpret_cast<void*>(static_cast<std::intptr_t>(i + 1)));
        }
    }
    log_.write("tray_create_end", fields({kv("t", elapsed(), 2), mac::focusSnapshot(panel_)}));
}

// The start delay is over: place the window, play the "go" sound and start the stage.
void ProbeRun::go() {
    went_ = true;
    mac::rememberFrontmost();
    std::string placement = "placed=0";
    if (hasWindow()) {
        placement = mac::placeWindow(cocoaWindow(), kPanelMargin, opt_.displayIndex);
    }
    log_.write("go", fields({kv("t", elapsed(), 2), placement, mac::rememberedLabel(),
                             mac::displaysSnapshot()}));
    if (opt_.sound) {
        mac::playSound("Glass");
    }
    if (!usesPanel()) {
        if (hasWindow()) {
            SDL_ShowWindow(window_);
            logFocus("plain_window_shown");
        } else {
            logFocus("detect_go");
        }
    } else {
        if (cycling()) {
            lastStep_ = 0;
            comboIndex_ = 0;
        }
        applyActiveCombo(false, false);
    }
    redraw_ = kFramesAfterEvent;
}

void ProbeRun::handleHotkey(int id) {
    log_.write("hotkey", fields({kv("id", static_cast<long long>(id)), kv("t", elapsed(), 2)}));
    logFocus("hotkey");
    if (id == 3) {
        quit("hotkey");
        return;
    }
    if (!interactiveStage() || !went_) {
        return;  // K, L and H only act in the interactive and tray stages
    }
    if (id == 1) {
        if (panelShown_) {
            dismissPanel("hotkey");
        } else {
            showPanel("hotkey");
        }
    } else if (id == 2) {
        setClickThrough(!clickThrough_, "hotkey");
    } else if (id == 4) {
        reactivate("hotkey");
    }
}

void ProbeRun::handleTray(int id) {
    log_.write("tray_entry", fields({kv("id", static_cast<long long>(id)), kv("t", elapsed(), 2)}));
    logFocus("tray_entry");
    if (id == 1) {
        showPanel("tray");
    } else if (id == 2) {
        dismissPanel("tray");
    } else if (id == 3) {
        quit("tray");
    }
}

void ProbeRun::handleEvent(const SDL_Event& event) {
    if (event.type == gEvents.tick) {
        handleTick();
        return;
    }
    if (event.type == gEvents.hotkey) {
        handleHotkey(event.user.code);
        redraw_ = kFramesAfterEvent;
        return;
    }
    if (event.type == gEvents.tray) {
        handleTray(event.user.code);
        redraw_ = kFramesAfterEvent;
        return;
    }

    if (backend_ != nullptr) {
        backend_->processEvent(event);
    }
    redraw_ = kFramesAfterEvent;

    switch (event.type) {
    case SDL_EVENT_QUIT:
        quit("sdl_quit");
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit("window_close_requested");
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        ++mouseDowns_;
        log_.write("mouse_down", fields({kv("button", static_cast<long long>(event.button.button)),
                                         kv("x", static_cast<double>(event.button.x), 0),
                                         kv("y", static_cast<double>(event.button.y), 0)}));
        logFocus("mouse_down");
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        log_.write("mouse_up", kv("button", static_cast<long long>(event.button.button)));
        break;
    case SDL_EVENT_KEY_DOWN: {
        ++keyDowns_;
        const char* keyName = SDL_GetKeyName(event.key.key);
        log_.write("key_down", fields({kv("key", keyName != nullptr ? keyName : "?"),
                                       kv("scancode", static_cast<long long>(event.key.scancode)),
                                       kv("mod", static_cast<long long>(event.key.mod)),
                                       kv("repeat", event.key.repeat ? 1LL : 0LL)}));
        if (keyName != nullptr && !event.key.repeat) {
            lastKeys_ += lastKeys_.empty() ? "" : " ";
            lastKeys_ += keyName;
            if (lastKeys_.size() > 60) {
                lastKeys_.erase(0, lastKeys_.size() - 60);
            }
        }
        if (event.key.scancode == SDL_SCANCODE_ESCAPE && panelShown_ && interactiveStage()) {
            dismissPanel("escape");
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        ++textInputs_;
        log_.write("text_input", kv("text", event.text.text != nullptr ? event.text.text : ""));
        break;
    default: {
        const char* name = windowEventName(event.type);
        if (name != nullptr) {
            log_.write(name, kv("t", elapsed(), 2));
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                logFocus(name);
            }
        }
        break;
    }
    }
}

void ProbeRun::handleTick() {
    if (sinceLaunch() >= opt_.maxSeconds) {
        quit("max_seconds");
        return;
    }
    if (!went_) {
        if (elapsed() >= 0.0) {
            go();
        }
        return;
    }

    const double t = elapsed();
    const double planned = runSeconds(opt_);
    if (planned > 0.0 && t >= planned) {
        quit("planned_seconds_done");
        return;
    }

    // Before a possible step change, so the state of the previous combination is logged first.
    if (settledLogAt_ >= 0.0 && t >= settledLogAt_) {
        settledLogAt_ = -1.0;
        logFocus("settled");
    }
    if (cycling()) {
        const int count = static_cast<int>(allCombos().size());
        const int step = comboIndexAt(t, opt_.stepSeconds, count + 1);  // the last step replays 1
        if (step >= 0 && step != lastStep_) {
            lastStep_ = step;
            comboIndex_ = step == count ? 0 : step;
            applyActiveCombo(step == count, true);
        }
    }
    if (clickThrough_ && t >= clickThroughUntil_) {
        setClickThrough(false, "timeout");
    }
    if (postActionLogAt_ >= 0.0 && t >= postActionLogAt_) {
        postActionLogAt_ = -1.0;
        logFocus(postActionLabel_);
    }
    if (opt_.stage == Stage::Tray && !trayCreated_ && t >= opt_.trayDelaySeconds) {
        createTray();
    }

    if (t >= nextStateLog_) {
        nextStateLog_ += kStateLogSeconds;
        if (opt_.stage == Stage::Detect) {
            log_.write("detect", fields({kv("t", t, 2), mac::ck3Snapshot(),
                                         mac::ck3WindowSnapshot(), mac::displaysSnapshot(),
                                         mac::focusSnapshot(nullptr), mac::permissionSnapshot(),
                                         kv("front_is_ck3", mac::frontmostIsCk3() ? 1LL : 0LL)}));
        } else if (opt_.stage != Stage::E4) {  // E4 keeps its own wake-ups to a minimum
            const std::string snapshot = mac::focusSnapshot(panel_);
            if (snapshot != lastFocus_) {
                lastFocus_ = snapshot;
                log_.write("focus", fields({kv("why", "changed"), kv("t", t, 2), snapshot}));
                dirty_ = true;
            }
        }
    }
    if (opt_.stage == Stage::E4 && t >= nextCpuLog_) {
        nextCpuLog_ += kCpuLogSeconds;
        logCpu("periodic");
    }
}

void ProbeRun::drawBanner() {
    const ImGuiIO& io = ImGui::GetIO();
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    const auto& combos = allCombos();
    const Combo& combo = combos[static_cast<std::size_t>(comboIndex_)];
    const float hue = opt_.stage == Stage::E3
                          ? static_cast<float>(combo.number - 1) / static_cast<float>(combos.size())
                          : (opt_.stage == Stage::E1 ? 0.02F : 0.58F);
    ImGui::ColorConvertHSVtoRGB(hue, 0.6F, 0.45F, r, g, b);

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(r, g, b, 1.0F));
    ImGui::Begin("##banner", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(nullptr, 34.0F);
    if (opt_.stage == Stage::E3) {
        ImGui::Text("COMBO %d / %d%s", combo.number, static_cast<int>(combos.size()),
                    lastStep_ == static_cast<int>(combos.size()) ? " (replay)" : "");
        ImGui::PopFont();
        ImGui::Text("level %d (%s)", combo.level, combo.levelName);
        ImGui::Text("flags %c:", combo.flagSet);
        ImGui::TextWrapped("%s", flagNames(combo.flags).c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Note the number above when you can see this window over CK3. If you see nothing, "
            "count the sounds after the first (higher) one: the log has the timeline.");
    } else {
        ImGui::TextUnformatted(opt_.stage == Stage::E1 ? "E1: plain window" : "E2: plain window");
        ImGui::PopFont();
        ImGui::TextUnformatted(opt_.stage == Stage::E1
                                   ? "Regular activation policy, always on top."
                                   : "Accessory activation policy, always on top.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "If you can read this over CK3: visible. Also note whether CK3 left its Space or the "
            "desktop Space appeared.");
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void ProbeRun::drawTransparentTest() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.0F);
    ImGui::Begin("##e4", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 8; ++i) {
        const float alpha = static_cast<float>(i + 1) / 8.0F;
        const ImVec2 a(origin.x + 20.0F + static_cast<float>(i) * 60.0F, origin.y + 20.0F);
        const ImVec2 b(a.x + 50.0F, a.y + 80.0F);
        list->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(1.0F, 0.63F, 0.0F, alpha)));
    }
    list->AddRectFilled(ImVec2(origin.x + 20.0F, origin.y + 130.0F),
                        ImVec2(origin.x + 260.0F, origin.y + 190.0F), IM_COL32(20, 20, 20, 255));
    list->AddRect(ImVec2(origin.x + 280.0F, origin.y + 130.0F),
                  ImVec2(origin.x + 520.0F, origin.y + 190.0F), IM_COL32(255, 255, 255, 255), 0.0F,
                  0, 2.0F);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 20.0F, origin.y + 210.0F));
    ImGui::TextUnformatted("E4: transparent panel. Outside the shapes the game must show.");
    ImGui::TextUnformatted("Look for a dark fringe around the bars and the outline.");
    ImGui::End();
}

void ProbeRun::drawInteractive() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.72F);
    ImGui::Begin("##interactive", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const Combo& combo = allCombos()[static_cast<std::size_t>(comboIndex_)];
    ImGui::Text("%s  combo %d (level %d)", opt_.stage == Stage::Tray ? "TRAY" : "INTERACTIVE",
                combo.number, combo.level);
    ImGui::Separator();
    ImGui::TextUnformatted("Type here:");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##text", textBuffer_, sizeof textBuffer_);
    ImGui::Text("mouse downs %d  key downs %d  text inputs %d", mouseDowns_, keyDowns_,
                textInputs_);
    ImGui::TextWrapped("last keys: %s", lastKeys_.c_str());
    if (ImGui::Button("Click-through 10 s")) {
        setClickThrough(true, "button");
    }
    ImGui::SameLine();
    if (ImGui::Button("Dismiss (Esc)")) {
        dismissPanel("button");
    }
    if (ImGui::Button("Re-activate previous app")) {
        reactivate("button");
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit")) {
        quit("button");
    }
    ImGui::TextWrapped(
        "Ctrl+Cmd+K show/hide, L click-through, H re-activate previous app, J quit.");
    ImGui::TextWrapped("%s", hotkeyStatus_.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("%s", lastFocus_.c_str());
    ImGui::End();
}

void ProbeRun::draw() {
    backend_->beginFrame();
    switch (opt_.stage) {
    case Stage::E1:
    case Stage::E2:
    case Stage::E3:
        drawBanner();
        break;
    case Stage::E4:
        drawTransparentTest();
        break;
    case Stage::Interactive:
    case Stage::Tray:
        drawInteractive();
        break;
    case Stage::Detect:
        break;
    }
    backend_->endFrame();
    ++frames_;
}

int ProbeRun::run() {
    if (!openLog()) {
        return 1;
    }
    delaySeconds_ = hasWindow() ? opt_.startDelaySeconds : 0.0;
    log_.write(
        "start",
        fields({kv("stage", stageName(opt_.stage)), kv("combo", static_cast<long long>(opt_.combo)),
                kv("step_seconds", opt_.stepSeconds, 1), kv("start_delay", delaySeconds_, 1),
                kv("planned_seconds", runSeconds(opt_), 1), kv("max_seconds", opt_.maxSeconds, 0),
                kv("sound", opt_.sound ? 1LL : 0LL),
                kv("display", static_cast<long long>(opt_.displayIndex)), kv("log", log_.path())}));

    // One probe at a time: the hot keys can be registered by only one process, and a second panel
    // would be hard to tell from the first.
    const std::vector<int> others = mac::otherProbePids();
    if (!others.empty()) {
        log_.write("another_instance_running", kv("pid", static_cast<long long>(others.front())));
        mac::playSound("Basso");
        return 3;
    }

    // Panel stages: do not let SDL apply the Regular policy or activate the app when a window is
    // shown (E1 and E2 keep SDL's defaults, they are the control).
    if (usesPanel() || opt_.stage == Stage::Detect) {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_.write("sdl_init_failed", kv("error", SDL_GetError()));
        return 1;
    }
    const int version = SDL_GetVersion();
    log_.write("sdl",
               fields({kv("version", std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." +
                                         std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
                                         std::to_string(SDL_VERSIONNUM_MICRO(version))),
                       kv("video_driver", SDL_GetCurrentVideoDriver() != nullptr
                                              ? SDL_GetCurrentVideoDriver()
                                              : "none")}));
    log_.write("policy_default", kv("policy", mac::activationPolicyName()));
    if (opt_.stage != Stage::E1) {
        const bool changed = mac::setAccessoryPolicy();
        log_.write("policy_set", fields({kv("accessory_ok", changed ? 1LL : 0LL),
                                         kv("policy", mac::activationPolicyName())}));
    }
    log_.write("environment", mac::environmentSnapshot());
    log_.write("displays", mac::displaysSnapshot());
    log_.write("permissions_start", mac::permissionSnapshot());
    log_.write("ck3", mac::ck3Snapshot());

    gEvents.tick = SDL_RegisterEvents(3);
    if (gEvents.tick == 0) {
        log_.write("register_events_failed", kv("error", SDL_GetError()));
        SDL_Quit();
        return 1;
    }
    gEvents.hotkey = gEvents.tick + 1;
    gEvents.tray = gEvents.tick + 2;
    mac::installWorkspaceObservers(
        [this](const std::string& line) { log_.write("workspace", line); });
    {
        std::string error;
        const bool ok =
            mac::registerHotkeys([](int id) { pushCustom(gEvents.hotkey, id); }, &error);
        hotkeyStatus_ = ok ? "hot keys registered" : "hot keys FAILED: " + error;
        log_.write("hotkeys_registered", fields({kv("ok", ok ? 1LL : 0LL), kv("errors", error)}));
    }

    if (opt_.combo > 0) {
        comboIndex_ = opt_.combo - 1;
    }

    if (hasWindow()) {
        if (!createWindow()) {
            log_.write("window_failed", kv("error", SDL_GetError()));
            mac::removeWorkspaceObservers();
            SDL_Quit();
            return 1;
        }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        // Content scale only: on macOS the pixel density is already applied by the renderer
        // backend through the framebuffer scale, so scaling by SDL_GetWindowDisplayScale would
        // double the UI.
        float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window_));
        if (scale <= 0.0F) {
            scale = 1.0F;
        }
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleDpi = scale;

        backend_ = curia::ui::createSdlRendererBackend();
        if (!backend_->init(window_)) {
            log_.write("backend_init_failed", kv("error", SDL_GetError()));
            ImGui::DestroyContext();
            SDL_DestroyWindow(window_);
            mac::releasePanel(panel_);
            mac::removeWorkspaceObservers();
            SDL_Quit();
            return 1;
        }
        backend_->setClearColor(0, 0, 0, transparentStage() ? 0 : 255);
        log_.write("window", fields({kv("renderer", backend_->rendererName()),
                                     kv("content_scale", static_cast<double>(scale), 2),
                                     kv("window_display_scale",
                                        static_cast<double>(SDL_GetWindowDisplayScale(window_)), 2),
                                     kv("panel", panel_ != nullptr ? 1LL : 0LL)}));
    }

    const SDL_TimerID timer = SDL_AddTimer(tickMs(), tickCallback, nullptr);
    if (timer == 0) {
        log_.write("add_timer_failed", kv("error", SDL_GetError()));
        running_ = false;
    }

    // Event-driven like the real app: draw two frames after each real event, otherwise block.
    while (running_) {
        SDL_Event event;
        bool have = false;
        if (redraw_ > 0) {
            have = SDL_PollEvent(&event);
        } else if (!SDL_WaitEvent(&event)) {
            log_.write("wait_event_failed", kv("error", SDL_GetError()));
            break;
        } else {
            have = true;
        }
        while (have) {
            handleEvent(event);
            have = SDL_PollEvent(&event);
        }
        if (dirty_) {
            redraw_ = kFramesAfterEvent;
            dirty_ = false;
        }
        if (running_ && redraw_ > 0) {
            if (drawable()) {
                draw();
            }
            --redraw_;
        }
    }

    logCpu("end");
    log_.write("permissions_end", mac::permissionSnapshot());
    logFocus("end");
    log_.write("end", fields({kv("t", elapsed(), 2), kv("frames", static_cast<long long>(frames_)),
                              kv("mouse_downs", static_cast<long long>(mouseDowns_)),
                              kv("key_downs", static_cast<long long>(keyDowns_)),
                              kv("text_inputs", static_cast<long long>(textInputs_))}));

    if (timer != 0) {
        SDL_RemoveTimer(timer);
    }
    mac::removeWorkspaceObservers();
    if (tray_ != nullptr) {
        SDL_DestroyTray(tray_);
    }
    if (backend_ != nullptr) {
        backend_->shutdown();
        backend_.reset();
        ImGui::DestroyContext();
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
    mac::releasePanel(panel_);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const ParseResult parsed = parseArgs(args);
    if (!parsed.ok) {
        std::fprintf(stderr, "curia_m2_probe: %s\n", parsed.error.c_str());
        // Started with `open` there is no terminal: leave the reason where the other logs are.
        const std::string dir = mac::homeDirectory() + "/curia_m2_results";
        std::error_code error;
        fs::create_directories(dir, error);
        ProbeLog log;
        if (log.open(dir + "/probe-error-" + timestampForFileName() + ".log", true)) {
            log.write("argument_error", kv("error", parsed.error));
        }
        return 2;
    }
    ProbeRun run(parsed.options);
    return run.run();
}
