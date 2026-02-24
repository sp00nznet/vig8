// vig8 - Menu bar and config dialogs implementation

#include "menu.h"
#include "settings.h"

#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/runtime.h>
#include <rex/kernel/kernel_state.h>
#include <rex/input/input_system.h>
#include <rex/input/input.h>
#include <rex/stream.h>

#include <imgui.h>

#include <fstream>
#include <vector>

using namespace rex::ui;

// ============================================================================
// Dialog classes
// ============================================================================

// Helpers for consistent button layout
static void RightAlignedButtons(float button_width = 80.0f) {
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - button_width * 2 - 8);
}

class GraphicsDialog : public ImGuiDialog {
public:
    GraphicsDialog(ImGuiDrawer* drawer, Window* window,
                   Vig8Settings* settings,
                   const std::filesystem::path& settings_path,
                   std::function<void()> on_done)
        : ImGuiDialog(drawer), window_(window), settings_(settings),
          settings_path_(settings_path), on_done_(std::move(on_done)) {
        render_path_idx_ = (settings->render_path == "rtv") ? 1 : 0;
        resolution_scale_idx_ = (settings->resolution_scale >= 2) ? 1 : 0;
        fullscreen_ = settings->fullscreen;
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        (void)io;
        ImGui::SetNextWindowSize(ImVec2(400, 220), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Graphics##vig8", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize)) {
            ImGui::Text("Render Path:");
            ImGui::SameLine(160);
            ImGui::SetNextItemWidth(180);
            const char* path_items[] = {"ROV (Recommended)", "RTV"};
            ImGui::Combo("##render_path", &render_path_idx_, path_items, 2);

            ImGui::Text("Resolution Scale:");
            ImGui::SameLine(160);
            ImGui::SetNextItemWidth(180);
            const char* scale_items[] = {"1x", "2x"};
            ImGui::Combo("##resolution_scale", &resolution_scale_idx_, scale_items, 2);

            ImGui::Checkbox("Fullscreen", &fullscreen_);

            ImGui::Spacing();
            ImGui::TextDisabled("Render path and resolution scale require restart.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RightAlignedButtons();
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                settings_->render_path = (render_path_idx_ == 0) ? "rov" : "rtv";
                settings_->resolution_scale = (resolution_scale_idx_ == 0) ? 1 : 2;
                bool fs_changed = (settings_->fullscreen != fullscreen_);
                settings_->fullscreen = fullscreen_;
                SaveSettings(settings_path_, *settings_);
                if (fs_changed && window_) {
                    window_->SetFullscreen(fullscreen_);
                }
                Close();
                if (on_done_) on_done_();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                Close();
                if (on_done_) on_done_();
            }
        }
        ImGui::End();
    }

private:
    Window* window_;
    Vig8Settings* settings_;
    std::filesystem::path settings_path_;
    std::function<void()> on_done_;
    int render_path_idx_ = 0;
    int resolution_scale_idx_ = 0;
    bool fullscreen_ = false;
};

class GameDialog : public ImGuiDialog {
public:
    GameDialog(ImGuiDrawer* drawer, Vig8Settings* settings,
               const std::filesystem::path& settings_path,
               std::function<void()> on_done)
        : ImGuiDialog(drawer), settings_(settings),
          settings_path_(settings_path), on_done_(std::move(on_done)) {
        full_game_ = settings->full_game;
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        (void)io;
        ImGui::SetNextWindowSize(ImVec2(350, 140), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Game Options##vig8", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize)) {
            ImGui::Checkbox("Unlock full game (skip trial mode)", &full_game_);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RightAlignedButtons();
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                settings_->full_game = full_game_;
                SaveSettings(settings_path_, *settings_);
                Close();
                if (on_done_) on_done_();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                Close();
                if (on_done_) on_done_();
            }
        }
        ImGui::End();
    }

private:
    Vig8Settings* settings_;
    std::filesystem::path settings_path_;
    std::function<void()> on_done_;
    bool full_game_ = true;
};

class DebugDialog : public ImGuiDialog {
public:
    DebugDialog(ImGuiDrawer* drawer, Vig8Settings* settings,
                const std::filesystem::path& settings_path,
                std::function<void()> on_done)
        : ImGuiDialog(drawer), settings_(settings),
          settings_path_(settings_path), on_done_(std::move(on_done)) {
        show_fps_ = settings->show_fps;
        show_console_ = settings->show_console;
        invulnerable_ = settings->invulnerable;
        unlock_all_cars_ = settings->unlock_all_cars;
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        (void)io;
        ImGui::SetNextWindowSize(ImVec2(350, 210), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Debug Options##vig8", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize)) {
            ImGui::Checkbox("Show FPS overlay", &show_fps_);
            ImGui::Checkbox("Show debug console", &show_console_);
            ImGui::Separator();
            ImGui::Checkbox("Player invulnerable", &invulnerable_);
            ImGui::Checkbox("Unlock all vehicles", &unlock_all_cars_);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RightAlignedButtons();
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                settings_->show_fps = show_fps_;
                settings_->show_console = show_console_;
                settings_->invulnerable = invulnerable_;
                settings_->unlock_all_cars = unlock_all_cars_;
                SaveSettings(settings_path_, *settings_);
                Close();
                if (on_done_) on_done_();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                Close();
                if (on_done_) on_done_();
            }
        }
        ImGui::End();
    }

private:
    Vig8Settings* settings_;
    std::filesystem::path settings_path_;
    std::function<void()> on_done_;
    bool show_fps_ = true;
    bool show_console_ = false;
    bool invulnerable_ = false;
    bool unlock_all_cars_ = false;
};

class ControlsDialog : public ImGuiDialog {
public:
    ControlsDialog(ImGuiDrawer* drawer, rex::Runtime* runtime,
                   Vig8Settings* settings,
                   const std::filesystem::path& settings_path,
                   std::function<void()> on_done)
        : ImGuiDialog(drawer), runtime_(runtime), settings_(settings),
          settings_path_(settings_path), on_done_(std::move(on_done)) {
        slots_[0] = settings->controller_1;
        slots_[1] = settings->controller_2;
        slots_[2] = settings->controller_3;
        slots_[3] = settings->controller_4;
        for (int i = 0; i < 4; i++) {
            if (slots_[i] == "keyboard") slot_idx_[i] = 2;
            else if (slots_[i] == "none") slot_idx_[i] = 1;
            else slot_idx_[i] = 0;  // "auto"
        }
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        (void)io;
        ImGui::SetNextWindowSize(ImVec2(420, 230), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls##vig8", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize)) {

            // Detect connected controllers
            bool connected[4] = {};
            if (runtime_ && runtime_->kernel_state()) {
                auto* input = runtime_->kernel_state()->input_system();
                if (input) {
                    for (int i = 0; i < 4; i++) {
                        rex::input::X_INPUT_CAPABILITIES caps = {};
                        connected[i] = (input->GetCapabilities(i, 0, &caps) == 0);
                    }
                }
            }

            if (ImGui::BeginTable("##controllers", 3,
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                const char* mode_items[] = {"Auto", "None", "Keyboard"};
                for (int i = 0; i < 4; i++) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Controller %d", i + 1);
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(140);
                    ImGui::Combo("##mode", &slot_idx_[i], mode_items, 3);
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    if (connected[i]) {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                           "Connected");
                    } else {
                        ImGui::TextDisabled("---");
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            RightAlignedButtons();
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                const char* values[] = {"auto", "none", "keyboard"};
                settings_->controller_1 = values[slot_idx_[0]];
                settings_->controller_2 = values[slot_idx_[1]];
                settings_->controller_3 = values[slot_idx_[2]];
                settings_->controller_4 = values[slot_idx_[3]];
                SaveSettings(settings_path_, *settings_);
                Close();
                if (on_done_) on_done_();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                Close();
                if (on_done_) on_done_();
            }
        }
        ImGui::End();
    }

private:
    rex::Runtime* runtime_;
    Vig8Settings* settings_;
    std::filesystem::path settings_path_;
    std::function<void()> on_done_;
    std::string slots_[4];
    int slot_idx_[4] = {};
};

// ============================================================================
// MenuSystem implementation
// ============================================================================

struct MenuSystem::Impl {
    ImGuiDrawer* imgui_drawer;
    Window* window;
    WindowedAppContext* app_context;
    rex::Runtime* runtime;
    Vig8Settings* settings;
    std::filesystem::path settings_path;
    std::function<void()> on_settings_changed;

    // Active dialog tracking
    GraphicsDialog* gfx_dialog = nullptr;
    GameDialog* game_dialog = nullptr;
    DebugDialog* debug_dialog = nullptr;
    ControlsDialog* controls_dialog = nullptr;

    template <typename T>
    std::function<void()> MakeOnDone(T*& tracking_ptr, bool notify = true) {
        return [this, &tracking_ptr, notify]() {
            app_context->CallInUIThreadDeferred([this, &tracking_ptr, notify]() {
                tracking_ptr = nullptr;
                if (notify && on_settings_changed) on_settings_changed();
            });
        };
    }

    void ShowGraphicsDialog() {
        if (gfx_dialog) return;
        gfx_dialog = new GraphicsDialog(imgui_drawer, window, settings,
                                        settings_path,
                                        MakeOnDone(gfx_dialog));
    }

    void ShowGameDialog() {
        if (game_dialog) return;
        game_dialog = new GameDialog(imgui_drawer, settings, settings_path,
                                     MakeOnDone(game_dialog));
    }

    void ShowDebugDialog() {
        if (debug_dialog) return;
        debug_dialog = new DebugDialog(imgui_drawer, settings, settings_path,
                                       MakeOnDone(debug_dialog));
    }

    void ShowControlsDialog() {
        if (controls_dialog) return;
        controls_dialog = new ControlsDialog(
            imgui_drawer, runtime, settings, settings_path,
            MakeOnDone(controls_dialog, false));
    }

    void ShowAbout() {
        ImGuiDialog::ShowMessageBox(
            imgui_drawer,
            "About Vigilante 8 Arcade",
            "Vigilante 8 Arcade - Static Recompilation\n\n"
            "Built with ReXGlue SDK\n"
            "https://github.com/sp00nznet/vig8");
    }

    void SaveState() {
        if (!runtime || !runtime->kernel_state()) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Save State",
                                        "Runtime not available.");
            return;
        }

        // Allocate a large buffer for state serialization
        constexpr size_t kMaxStateSize = 256 * 1024 * 1024;  // 256 MB
        std::vector<uint8_t> buffer(kMaxStateSize);
        rex::stream::ByteStream stream(buffer.data(), buffer.size());

        if (!runtime->kernel_state()->Save(&stream)) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Save State",
                                        "Failed to save state.");
            return;
        }

        // Write only the bytes that were used
        auto save_path = settings_path.parent_path() / "vig8_savestate.bin";
        std::ofstream f(save_path, std::ios::binary);
        if (!f) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Save State",
                                        "Failed to open save file.");
            return;
        }
        f.write(reinterpret_cast<const char*>(buffer.data()), stream.offset());
        f.close();

        ImGuiDialog::ShowMessageBox(
            imgui_drawer, "Save State",
            ("State saved to " + save_path.filename().string() +
             " (" + std::to_string(stream.offset() / 1024) + " KB)").c_str());
    }

    void LoadState() {
        if (!runtime || !runtime->kernel_state()) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Load State",
                                        "Runtime not available.");
            return;
        }

        auto save_path = settings_path.parent_path() / "vig8_savestate.bin";
        std::ifstream f(save_path, std::ios::binary | std::ios::ate);
        if (!f) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Load State",
                                        "No save state file found.");
            return;
        }

        auto size = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buffer(size);
        f.read(reinterpret_cast<char*>(buffer.data()), size);
        f.close();

        rex::stream::ByteStream stream(buffer.data(), buffer.size());
        if (!runtime->kernel_state()->Restore(&stream)) {
            ImGuiDialog::ShowMessageBox(imgui_drawer, "Load State",
                                        "Failed to restore state.");
            return;
        }

        ImGuiDialog::ShowMessageBox(imgui_drawer, "Load State",
                                    "State restored successfully.");
    }
};

MenuSystem::MenuSystem(ImGuiDrawer* imgui_drawer, Window* window,
                       WindowedAppContext* app_context,
                       rex::Runtime* runtime,
                       Vig8Settings* settings,
                       const std::filesystem::path& settings_path,
                       std::function<void()> on_settings_changed)
    : impl_(std::make_unique<Impl>()) {
    impl_->imgui_drawer = imgui_drawer;
    impl_->window = window;
    impl_->app_context = app_context;
    impl_->runtime = runtime;
    impl_->settings = settings;
    impl_->settings_path = settings_path;
    impl_->on_settings_changed = std::move(on_settings_changed);
}

MenuSystem::~MenuSystem() = default;

std::unique_ptr<MenuItem> MenuSystem::BuildMenuBar() {
    auto* ctx = impl_.get();

    // Root menu bar
    auto root = MenuItem::Create(MenuItem::Type::kNormal);

    // --- File menu ---
    auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "File");

    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Save State...",
        [ctx]() { ctx->SaveState(); }));

    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Load State...",
        [ctx]() { ctx->LoadState(); }));

    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));

    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Exit",
        [ctx]() { ctx->app_context->QuitFromUIThread(); }));

    root->AddChild(std::move(file_menu));

    // --- Config menu ---
    auto config_menu = MenuItem::Create(MenuItem::Type::kPopup, "Config");

    config_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Controls...",
        [ctx]() { ctx->ShowControlsDialog(); }));

    config_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Graphics...",
        [ctx]() { ctx->ShowGraphicsDialog(); }));

    config_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Game...",
        [ctx]() { ctx->ShowGameDialog(); }));

    config_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Debug...",
        [ctx]() { ctx->ShowDebugDialog(); }));

    root->AddChild(std::move(config_menu));

    // --- Help menu ---
    auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "Help");

    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "About...",
        [ctx]() { ctx->ShowAbout(); }));

    root->AddChild(std::move(help_menu));

    return root;
}
