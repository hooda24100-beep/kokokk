#include <windows.h>
#include <dwmapi.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <algorithm>
#include <string>

#include "src/config/Config.h"
#include "src/core/CaptureDXGI.h"
#include "src/core/Scanner.h"
#include "src/input/InputDispatcher.h"
#include "src/ui/Menu.h"
#include "src/utils/ThreadPool.h"

static HWND g_hwnd = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_workerRunning{true};
static int g_screenW = 0;
static int g_screenH = 0;

static const int MIN_MT_PIXELS = 65536;
/// Brief centroid hold when pixels drop for a few frames — kills flicker unlock without ramping confidence.
static const int TRACK_HYSTERESIS_FRAMES = 3;

static void WorkerLoop() {
    using Clock = std::chrono::steady_clock;
    float remainderX = 0.0f, remainderY = 0.0f;
    auto  fireCooldownEnd = Clock::now();
    bool  wasEnabled = false;

    float       lastRgb[3]  = {-1.0f, -1.0f, -1.0f};
    HSVTarget   cachedHSV{};

    float ghostX = 0.0f, ghostY = 0.0f;
    int   ghostLeft = 0;

    int              tileAllocForFov = 0;
    std::unique_ptr<TileResult[]> tileResults;

    while (g_running.load(std::memory_order_relaxed) && g_workerRunning.load(std::memory_order_relaxed)) {
        auto frameStart = Clock::now();

        BotConfig cfg;
        { std::lock_guard<std::mutex> lk(g_configMutex); cfg = g_config; }

        const bool isEnabled = cfg.enabled && cfg.fovRadius >= 1;

        if (!isEnabled) {
            if (wasEnabled) {
                remainderX = remainderY = 0.0f;
                ghostLeft = 0;
                lastRgb[0] = lastRgb[1] = lastRgb[2] = -1.0f;
            }
            wasEnabled = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (!wasEnabled) {
            CaptureDXGI::ForceReset();
            wasEnabled = true;
        }

        POINT cursorRoi{};
        GetCursorPos(&cursorRoi);

        auto captureStart = Clock::now();
        const uint8_t* mappedPtr = nullptr;
        UINT mappedPitch = 0;

        const int effectiveFov = std::clamp(cfg.fovRadius, kFovRadiusMin, kFovRadiusMax);
        const int boxL = std::max<int>(0, static_cast<int>(cursorRoi.x) - effectiveFov);
        const int boxT = std::max<int>(0, static_cast<int>(cursorRoi.y) - effectiveFov);
        const int subW = std::min<int>(g_screenW, static_cast<int>(cursorRoi.x) + effectiveFov + 1) - boxL;
        const int subH = std::min<int>(g_screenH, static_cast<int>(cursorRoi.y) + effectiveFov + 1) - boxT;

        bool frameOk = false;
        if (subW > 0 && subH > 0) {
            frameOk = CaptureDXGI::AcquireFrame(boxL, boxT, subW, subH, mappedPtr, mappedPitch);
        }

        auto captureEnd = Clock::now();
        g_perf.captureTimeMs.store(std::chrono::duration<float, std::milli>(captureEnd - captureStart).count(), std::memory_order_relaxed);

        if (!frameOk || !mappedPtr) {
            if (CaptureDXGI::ConsumeLastAcquireWasTimeout())
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto scanStart = Clock::now();
        if (cfg.targetColor[0] != lastRgb[0] || cfg.targetColor[1] != lastRgb[1] || cfg.targetColor[2] != lastRgb[2]) {
            lastRgb[0] = cfg.targetColor[0];
            lastRgb[1] = cfg.targetColor[1];
            lastRgb[2] = cfg.targetColor[2];
            cachedHSV = ComputeTargetHSV(cfg.targetColor);
        }
        const HSVTarget& target = cachedHSV;
        const int hsvTolerance = std::clamp(cfg.cbcrThreshold, 5, 50);

        const int needDim = 2 * effectiveFov + 1;
        const int needTilesPerDim = (needDim + 7) / 8 + 1;
        const int needTiles = needTilesPerDim * needTilesPerDim;
        if (!tileResults || effectiveFov > tileAllocForFov) {
            tileAllocForFov = effectiveFov;
            tileResults = std::make_unique<TileResult[]>(std::max(needTiles, 1));
        }

        ScanResult result;
        const int totalPixels = subW * subH;

        if (totalPixels >= MIN_MT_PIXELS) {
            result = Scanner::ScanRegionMT(mappedPtr, mappedPitch, boxL, boxT, subW, subH, target, hsvTolerance, tileResults.get());
        } else {
            result = Scanner::ScanRegionFull(mappedPtr, mappedPitch, boxL, boxT, subW, subH, target, hsvTolerance);
        }

        const float minW = std::max(0.25f, cfg.minMatchWeight);
        const bool rawDetected = result.found() && result.sumW >= minW;
        float tgtX = -1.0f, tgtY = -1.0f;
        if (rawDetected) {
            tgtX = static_cast<float>(result.centroidX(cfg.offsetX));
            tgtY = static_cast<float>(result.centroidY(cfg.offsetY));
        }

        POINT cursorFrame{};
        GetCursorPos(&cursorFrame);

        auto scanEnd = Clock::now();
        g_perf.scanTimeMs.store(std::chrono::duration<float, std::milli>(scanEnd - scanStart).count(), std::memory_order_relaxed);
        g_perf.pixelsScanned.store(totalPixels, std::memory_order_relaxed);
        g_perf.stageUsed.store(rawDetected ? 1 : 0, std::memory_order_relaxed);
        g_perf.roiW.store(subW, std::memory_order_relaxed);
        g_perf.roiH.store(subH, std::memory_order_relaxed);
        g_perf.trackStrength.store(
            rawDetected ? std::min(1.0f, result.sumW / 25.0f) : (ghostLeft > 0 ? 0.35f : 0.0f),
            std::memory_order_relaxed);

        CaptureDXGI::ReleaseFrame();

        if (cfg.triggerbotEnabled && rawDetected) {
            const float dx = tgtX - static_cast<float>(cursorFrame.x);
            const float dy = tgtY - static_cast<float>(cursorFrame.y);
            const float r = static_cast<float>(cfg.triggerbotRadius);
            const float distSq = dx * dx + dy * dy;
            if (distSq <= r * r && Clock::now() >= fireCooldownEnd) {
                InputDispatcher::Click();
                if (cfg.triggerbotDelayMs > 0)
                    fireCooldownEnd = Clock::now() + std::chrono::milliseconds(cfg.triggerbotDelayMs);
                else
                    fireCooldownEnd = Clock::now();
            }
        }

        if (!cfg.aimbotEnabled) {
            g_perf.totalTimeMs.store(std::chrono::duration<float, std::milli>(Clock::now() - frameStart).count(), std::memory_order_relaxed);
            g_perf.tick();
            continue;
        }

        float aimX = 0.0f, aimY = 0.0f;
        bool aimActive = false;
        if (rawDetected) {
            ghostX = tgtX;
            ghostY = tgtY;
            ghostLeft = TRACK_HYSTERESIS_FRAMES;
            aimX = tgtX;
            aimY = tgtY;
            aimActive = true;
        } else if (ghostLeft > 0) {
            --ghostLeft;
            aimX = ghostX;
            aimY = ghostY;
            aimActive = true;
        } else {
            remainderX = remainderY = 0.0f;
        }

        if (!aimActive) {
            g_perf.totalTimeMs.store(std::chrono::duration<float, std::milli>(Clock::now() - frameStart).count(), std::memory_order_relaxed);
            g_perf.tick();
            continue;
        }

        float errorX = aimX - static_cast<float>(cursorFrame.x);
        float errorY = aimY - static_cast<float>(cursorFrame.y);

        remainderX += errorX;
        remainderY += errorY;

        const long dx = static_cast<long>(std::round(remainderX));
        const long dy = static_cast<long>(std::round(remainderY));

        remainderX -= static_cast<float>(dx);
        remainderY -= static_cast<float>(dy);

        InputDispatcher::Move(dx, dy);

        g_perf.totalTimeMs.store(std::chrono::duration<float, std::milli>(Clock::now() - frameStart).count(), std::memory_order_relaxed);
        g_perf.tick();
    }
}

#include "imgui.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return TRUE;
    if (msg == WM_DESTROY) {
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    Scanner::InitFeatureSupport();
    LoadConfig("v7_config.ini");
    InputDispatcher::Init();

    const std::wstring windowClass = L"V7Ctrl_" + std::to_wstring(GetCurrentProcessId());

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, windowClass.c_str(), nullptr};
    if (!RegisterClassExW(&wc))
        return EXIT_FAILURE;

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, windowClass.c_str(), L"V7", WS_POPUP, 0, 0, g_screenW, g_screenH, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        UnregisterClassW(windowClass.c_str(), hInst);
        return EXIT_FAILURE;
    }

    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (!CaptureDXGI::InitCore(g_hwnd, g_screenW, g_screenH)) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(windowClass.c_str(), hInst);
        return EXIT_FAILURE;
    }

    Menu::Init(g_hwnd);

    const int poolSize = std::min(4, static_cast<int>(std::thread::hardware_concurrency()));
    g_threadPool.start(std::max(1, poolSize));

    std::thread worker(WorkerLoop);

    MSG msg = {};
    bool prevToggle = false;

    while (g_running.load(std::memory_order_relaxed)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        BotConfig localCfg;
        { std::lock_guard<std::mutex> lk(g_configMutex); localCfg = g_config; }

        if (localCfg.holdMode) {
            bool held = (GetAsyncKeyState(localCfg.holdKey) & 0x8000) != 0;
            if (held != localCfg.enabled) {
                std::lock_guard<std::mutex> lk(g_configMutex);
                g_config.enabled = held;
            }
        } else {
            const bool toggleSuppressed = (localCfg.toggleKey == VK_INSERT);
            const bool tog = !toggleSuppressed &&
                (GetAsyncKeyState(localCfg.toggleKey) & 0x8000) != 0;
            if (tog && !prevToggle) {
                std::lock_guard<std::mutex> lk(g_configMutex);
                g_config.enabled = !g_config.enabled;
            }
            prevToggle = tog;
        }

        static bool prevMenu = false;
        const bool menu = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (menu && !prevMenu) {
            Menu::Toggle();
            LONG exStyle = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
            exStyle = Menu::IsOpen() ? (exStyle & ~WS_EX_TRANSPARENT) : (exStyle | WS_EX_TRANSPARENT);
            SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        prevMenu = menu;

        Menu::Render();

        bool fovOverlayActive = false;
        {
            std::lock_guard<std::mutex> lk(g_configMutex);
            fovOverlayActive = g_config.showFov && g_config.enabled;
        }
        if (fovOverlayActive)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SaveConfig("v7_config.ini");
    g_workerRunning = false;
    if (worker.joinable()) worker.join();
    g_threadPool.stop();
    Menu::Shutdown();
    CaptureDXGI::Shutdown();
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(windowClass.c_str(), hInst);
    return EXIT_SUCCESS;
}
