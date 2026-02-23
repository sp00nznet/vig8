// vig8 - ReXGlue Recompiled Project
// Vigilante 8 Arcade Static Recompilation

#include "vig8_config.h"
#include "vig8_init.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/kernel/xthread.h>
#include <rex/kernel/kernel_state.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>

// VEH handler: catch null page reads in guest memory and return 0
static LONG CALLBACK NullPageHandler(EXCEPTION_POINTERS* ep) {
    auto* ctx = ep->ContextRecord;
    auto* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != STATUS_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t addr = rec->ExceptionInformation[1];
    uint64_t base = ctx->Rsi;
    if (base >= 0x100000000ULL && base <= 0x200000000ULL &&
        addr >= base && addr < base + 0x10000) {
        uint8_t* rip = (uint8_t*)ctx->Rip;
        int rex = 0, oplen = 0;
        uint8_t op = rip[0];
        if ((op & 0xF0) == 0x40) { rex = op; op = rip[1]; oplen = 1; }
        if (op == 0x8B) {
            uint8_t modrm = rip[oplen + 1];
            int reg = (modrm >> 3) & 7;
            if (rex & 0x04) reg += 8;
            int mod = (modrm >> 6) & 3;
            int rm = modrm & 7;
            int insn_len = oplen + 2;
            if (rm == 4 && mod != 3) insn_len++;
            if (mod == 0 && rm == 5) insn_len += 4;
            else if (mod == 1) insn_len += 1;
            else if (mod == 2) insn_len += 4;
            uint64_t* regs[] = { &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
                                  &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
                                  &ctx->R8, &ctx->R9, &ctx->R10, &ctx->R11,
                                  &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15 };
            if (reg < 16) *regs[reg] = 0;
            ctx->Rip += insn_len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static struct NullPageGuard_ {
    NullPageGuard_() { AddVectoredExceptionHandler(1, NullPageHandler); }
} g_null_page_guard_;
#endif

class DebugOverlayDialog : public rex::ui::ImGuiDialog {
public:
    DebugOverlayDialog(rex::ui::ImGuiDrawer* imgui_drawer)
        : ImGuiDialog(imgui_drawer) {}
protected:
    void OnDraw(ImGuiIO& io) override {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
            ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
        }
        ImGui::End();
    }
};

class Vig8App : public rex::ui::WindowedApp, public rex::ui::WindowListener {
public:
    static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
        return std::make_unique<Vig8App>(ctx);
    }

    Vig8App(rex::ui::WindowedAppContext& ctx)
        : WindowedApp(ctx, "vig8", "[game_directory]") {
        AddPositionalOption("game_directory");
    }

    bool OnInitialize() override {
        auto exe_dir = rex::filesystem::GetExecutableFolder();

        // Game directory: arg or default to exe_dir/assets
        std::filesystem::path game_dir;
        if (auto arg = GetArgument("game_directory")) {
            game_dir = *arg;
        } else {
            game_dir = exe_dir / "assets";
        }

        std::string log_file_cvar = REXCVAR_GET(log_file);
        std::string log_level_str = REXCVAR_GET(log_level);
        if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
            log_level_str = "trace";
        }
        auto log_config = rex::BuildLogConfig(
            log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
            log_level_str, {});
        rex::InitLogging(log_config);
        rex::RegisterLogLevelCallback();
        REXLOG_INFO("vig8 starting");
        REXLOG_INFO("  Game directory: {}", game_dir.string());

        // Create and initialize runtime
        runtime_ = std::make_unique<rex::Runtime>(game_dir);
        runtime_->set_app_context(&app_context());

        auto status = runtime_->Setup(
            static_cast<uint32_t>(PPC_CODE_BASE),
            static_cast<uint32_t>(PPC_CODE_SIZE),
            static_cast<uint32_t>(PPC_IMAGE_BASE),
            static_cast<uint32_t>(PPC_IMAGE_SIZE),
            PPCFuncMappings);
        if (XFAILED(status)) {
            REXLOG_ERROR("Runtime setup failed: {:08X}", status);
            return false;
        }

        // Load XEX image
        status = runtime_->LoadXexImage("game:\\default.xex");
        if (XFAILED(status)) {
            REXLOG_ERROR("Failed to load XEX: {:08X}", status);
            return false;
        }

        // Create window
        window_ = rex::ui::Window::Create(app_context(), "Vigilante 8 Arcade", 1280, 720);
        if (!window_) {
            REXLOG_ERROR("Failed to create window");
            return false;
        }

        window_->AddListener(this);
        window_->Open();

        // Setup graphics presenter and ImGui
        auto* graphics_system = runtime_->graphics_system();
        if (graphics_system && graphics_system->presenter()) {
            auto* presenter = graphics_system->presenter();
            auto* provider = graphics_system->provider();
            if (provider) {
                immediate_drawer_ = provider->CreateImmediateDrawer();
                if (immediate_drawer_) {
                    immediate_drawer_->SetPresenter(presenter);
                    imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
                    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
                    debug_overlay_ = std::unique_ptr<DebugOverlayDialog>(
                        new DebugOverlayDialog(imgui_drawer_.get()));
                    runtime_->set_display_window(window_.get());
                    runtime_->set_imgui_drawer(imgui_drawer_.get());
                }
            }
            window_->SetPresenter(presenter);
        }

        // Launch module in background
        app_context().CallInUIThreadDeferred([this]() {
            auto main_thread = runtime_->LaunchModule();
            if (!main_thread) {
                REXLOG_ERROR("Failed to launch module");
                app_context().QuitFromUIThread();
                return;
            }

            module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
                main_thread->Wait(0, 0, 0, nullptr);
                REXLOG_INFO("Execution complete");
                if (!shutting_down_.load(std::memory_order_acquire)) {
                    app_context().CallInUIThread([this]() {
                        app_context().QuitFromUIThread();
                    });
                }
            });
        });

        return true;
    }

    void OnClosing(rex::ui::UIEvent& e) override {
        (void)e;
        REXLOG_INFO("Window closing, shutting down...");
        shutting_down_.store(true, std::memory_order_release);
        if (runtime_ && runtime_->kernel_state()) {
            runtime_->kernel_state()->TerminateTitle();
        }
        app_context().QuitFromUIThread();
    }

    void OnDestroy() override {
        // ImGui cleanup (reverse of setup)
        debug_overlay_.reset();
        if (imgui_drawer_) {
            imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
            imgui_drawer_.reset();
        }
        if (immediate_drawer_) {
            immediate_drawer_->SetPresenter(nullptr);
            immediate_drawer_.reset();
        }
        if (runtime_) {
            runtime_->set_display_window(nullptr);
            runtime_->set_imgui_drawer(nullptr);
        }
        // Window/runtime cleanup
        if (window_) {
            window_->SetPresenter(nullptr);
        }
        if (module_thread_.joinable()) {
            module_thread_.join();
        }
        if (window_) {
            window_->RemoveListener(this);
        }
        window_.reset();
        runtime_.reset();
    }

private:
    std::unique_ptr<rex::Runtime> runtime_;
    std::unique_ptr<rex::ui::Window> window_;
    std::thread module_thread_;
    std::atomic<bool> shutting_down_{false};
    std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
    std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
    std::unique_ptr<DebugOverlayDialog> debug_overlay_;
};

XE_DEFINE_WINDOWED_APP(vig8, Vig8App::Create)
