#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <atomic>
#include <random>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <stop_token> 

#define IDI_ICON_STOP 201
#define IDI_ICON_RUN  202

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

struct ClickerConfig {
    int64_t baseIntervalMs;
    int randomOffsetMs;
    bool useRandom;
    int buttonIndex;
    int typeIndex;

    ClickerConfig(int64_t h, int64_t m, int64_t s, int64_t ms, int offset, bool random, int btn, int type) {
        h = std::clamp<int64_t>(h, 0, 24);
        m = std::clamp<int64_t>(m, 0, 59);
        s = std::clamp<int64_t>(s, 0, 59);
        ms = std::clamp<int64_t>(ms, 0, 999);

        baseIntervalMs = (h * 3600000) + (m * 60000) + (s * 1000) + ms;
        if (baseIntervalMs <= 0) baseIntervalMs = 1;

        randomOffsetMs = std::max(0, std::abs(offset));
        if (randomOffsetMs >= baseIntervalMs) randomOffsetMs = (int)(baseIntervalMs - 1);
        
        useRandom = random && (randomOffsetMs > 0);
        buttonIndex = std::clamp(btn, 0, 2);
        typeIndex = std::clamp(type, 0, 2);
    }
};

class TimerResolutionGuard {
public:
    TimerResolutionGuard() {
        hWinMM = LoadLibraryW(L"winmm.dll");
        if (hWinMM) {
            pBegin = (Fn)GetProcAddress(hWinMM, "timeBeginPeriod");
            pEnd = (Fn)GetProcAddress(hWinMM, "timeEndPeriod");
            if (pBegin) pBegin(1);
        }
    }
    ~TimerResolutionGuard() {
        if (pEnd) pEnd(1);
        if (hWinMM) FreeLibrary(hWinMM);
    }
private:
    using Fn = UINT(WINAPI*)(UINT);
    HMODULE hWinMM = nullptr;
    Fn pBegin = nullptr;
    Fn pEnd = nullptr;
};

void SetThreadInfo(LPCWSTR name, int priority) {
    HANDLE hThread = GetCurrentThread();
    SetThreadPriority(hThread, priority);
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        auto pSetDesc = (HRESULT(WINAPI*)(HANDLE, PCWSTR))GetProcAddress(hKernel, "SetThreadDescription");
        if (pSetDesc) pSetDesc(hThread, name);
    }
}

class StochasticTimer {
public:
    explicit StochasticTimer(int64_t base, int offset) 
        : baseIntervalMs(base), randomOffsetMs(offset), drift(0.0) {
        std::random_device rd;
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::seed_seq ss{ (uint64_t)rd(), (uint64_t)now };
        gen = std::mt19937(ss);

        double stddev = (offset > 0) ? (offset / 3.0) : 1.0;
        dist = std::normal_distribution<double>(0.0, stddev);
    }

    int64_t NextDelay() {
        if (randomOffsetMs <= 0) return baseIntervalMs;
        double step = dist(gen);
        drift = 0.8 * drift + 0.2 * step;
        double noise = dist(gen) * 0.5;
        double finalOffset = std::clamp(drift + noise, (double)-randomOffsetMs, (double)randomOffsetMs);
        int64_t result = baseIntervalMs + (int64_t)std::round(finalOffset);
        return std::max<int64_t>(1, result);
    }

private:
    int64_t baseIntervalMs;
    int randomOffsetMs;
    std::mt19937 gen;
    std::normal_distribution<double> dist;
    double drift;
};

class InputInjector {
public:
    void ClickSingle(int btnIndex) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[1].type = INPUT_MOUSE;

        DWORD down = MOUSEEVENTF_LEFTDOWN;
        DWORD up = MOUSEEVENTF_LEFTUP;

        if (btnIndex == 1) { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
        else if (btnIndex == 2) { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }

        inputs[0].mi.dwFlags = down;
        inputs[1].mi.dwFlags = up;
        
        if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
            OutputDebugStringW(L"[Error] SendInput failed.\n");
        }
    }
};

class ClickerEngine {
public:
    ClickerEngine() : running(false) {}
    
    void Start(const ClickerConfig& config) {
        std::lock_guard<std::mutex> lock(mtxConfig);
        if (running.load()) return;
        running.store(true);
        workerThread = std::jthread([this, config](std::stop_token st) { Worker(st, config); });
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mtxConfig);
        if (!running.load()) return;
        workerThread.request_stop();
        cv.notify_all();
        running.store(false);
    }

    bool IsRunning() const { return running.load(); }

private:
    void Worker(std::stop_token stoken, ClickerConfig config) {
        SetThreadInfo(L"ClickerWorker", THREAD_PRIORITY_ABOVE_NORMAL);
        
        auto timer = std::make_unique<StochasticTimer>(config.baseIntervalMs, config.useRandom ? config.randomOffsetMs : 0);
        InputInjector injector;

        using clock = std::chrono::steady_clock;
        auto nextWake = clock::now();

        int sysDblTime = GetDoubleClickTime();
        int subClickDelay = (sysDblTime > 0) ? (sysDblTime / 5) : 50;
        int count = (config.typeIndex == 0) ? 1 : ((config.typeIndex == 1) ? 2 : 3);

        while (!stoken.stop_requested()) {
            int64_t delay = timer->NextDelay();
            nextWake += std::chrono::milliseconds(delay);
            
            std::unique_lock<std::mutex> lock(mtxCv);
            if (cv.wait_until(lock, nextWake, [&stoken] { return stoken.stop_requested(); })) return;
            
            for (int i = 0; i < count; ++i) {
                if (stoken.stop_requested()) return;
                injector.ClickSingle(config.buttonIndex);

                if (i < count - 1) {
                    auto subWake = clock::now() + std::chrono::milliseconds(subClickDelay);
                    if (cv.wait_until(lock, subWake, [&stoken] { return stoken.stop_requested(); })) return;
                }
            }
        }
    }

    std::atomic<bool> running;
    std::jthread workerThread;
    std::mutex mtxConfig;
    std::mutex mtxCv;
    std::condition_variable cv;
};

enum UI_ID {
    ID_EDIT_H = 101, ID_EDIT_M, ID_EDIT_S, ID_EDIT_MS,
    ID_CHK_RANDOM, ID_EDIT_RANDOM,
    ID_CMB_BTN, ID_CMB_TYPE,
    ID_BTN_HOTKEY, ID_BTN_START, ID_BTN_STOP
};
#define HOTKEY_ID 1

class AutoClickerApp {
public:
    AutoClickerApp(HINSTANCE hInst) : hInstance(hInst), vkHotkey(VK_F7), modHotkey(0), isListeningHotkey(false) {
        hbrDarkBg = CreateSolidBrush(RGB(30, 30, 30));
        hbrDarkEdit = CreateSolidBrush(RGB(45, 45, 48));
        hIconStop = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_ICON_STOP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
        hIconRun  = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_ICON_RUN),  IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    }

    ~AutoClickerApp() {
        if (hMainFont) DeleteObject(hMainFont);
        if (hbrDarkBg) DeleteObject(hbrDarkBg);
        if (hbrDarkEdit) DeleteObject(hbrDarkEdit);
    }

    int Run(int nCmdShow) {
        if (!hbrDarkBg || !hbrDarkEdit) return -1;

        WNDCLASSEXW wc = {0}; 
        wc.cbSize = sizeof(WNDCLASSEXW); 
        wc.lpfnWndProc = WndProcStatic;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"UltimateClickerClass";
        wc.hbrBackground = hbrDarkBg;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = hIconStop;
        wc.hIconSm = hIconStop;

        if (!RegisterClassExW(&wc)) return -1;

        int w = 285, h = 365;
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

        hwndMain = CreateWindowExW(0, wc.lpszClassName, L"kishiClicker",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            (sw - w) / 2, (sh - h) / 2, w, h, NULL, NULL, hInstance, this);

        if (!hwndMain) return -1;

        UpdateUIState(false); 

        ShowWindow(hwndMain, nCmdShow);
        UpdateWindow(hwndMain);

        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    HINSTANCE hInstance;
    HWND hwndMain = nullptr;
    HWND hEditH = nullptr, hEditM = nullptr, hEditS = nullptr, hEditMS = nullptr;
    HWND hChkRandom = nullptr, hEditRandom = nullptr;
    HWND hCmbBtn = nullptr, hCmbType = nullptr;
    HWND hBtnHotkey = nullptr, hBtnStart = nullptr, hBtnStop = nullptr, hTxtHotkey = nullptr;
    
    HBRUSH hbrDarkBg, hbrDarkEdit;
    HFONT hMainFont = nullptr;
    HICON hIconStop = nullptr;
    HICON hIconRun = nullptr;
    
    ClickerEngine engine;
    int vkHotkey;
    UINT modHotkey;
    bool isListeningHotkey;

    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        AutoClickerApp* app = nullptr;
        if (msg == WM_CREATE) {
            auto cs = (CREATESTRUCT*)lParam;
            app = (AutoClickerApp*)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            app->hwndMain = hwnd;
            app->InitControls(hwnd);
            app->LoadSettings();
            return 0;
        }
        app = (AutoClickerApp*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        return app ? app->HandleMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void InitControls(HWND hwnd) {
        hMainFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        CreateWindowW(L"STATIC", L"Interval (H/M/S/Millis):", WS_CHILD | WS_VISIBLE, 20, 20, 180, 20, hwnd, 0, hInstance, 0);
        hEditH = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, 20, 45, 40, 25, hwnd, (HMENU)ID_EDIT_H, hInstance, 0);
        hEditM = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, 70, 45, 40, 25, hwnd, (HMENU)ID_EDIT_M, hInstance, 0);
        hEditS = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, 120, 45, 40, 25, hwnd, (HMENU)ID_EDIT_S, hInstance, 0);
        hEditMS = CreateWindowExW(0, L"EDIT", L"100", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, 170, 45, 60, 25, hwnd, (HMENU)ID_EDIT_MS, hInstance, 0);

        hChkRandom = CreateWindowW(L"BUTTON", L"Random Offset (ms):", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 90, 150, 20, hwnd, (HMENU)ID_CHK_RANDOM, hInstance, 0);
        SendMessageW(hChkRandom, BM_SETCHECK, BST_CHECKED, 0);
        hEditRandom = CreateWindowExW(0, L"EDIT", L"40", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, 180, 88, 50, 25, hwnd, (HMENU)ID_EDIT_RANDOM, hInstance, 0);
        EnableWindow(hEditRandom, TRUE);

        CreateWindowW(L"STATIC", L"Mouse Button:", WS_CHILD | WS_VISIBLE, 20, 135, 100, 20, hwnd, 0, hInstance, 0);
        hCmbBtn = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 120, 132, 110, 100, hwnd, (HMENU)ID_CMB_BTN, hInstance, 0);
        SendMessageW(hCmbBtn, CB_ADDSTRING, 0, (LPARAM)L"Left");
        SendMessageW(hCmbBtn, CB_ADDSTRING, 0, (LPARAM)L"Right");
        SendMessageW(hCmbBtn, CB_ADDSTRING, 0, (LPARAM)L"Middle");
        SendMessageW(hCmbBtn, CB_SETCURSEL, 0, 0);

        CreateWindowW(L"STATIC", L"Click Type:", WS_CHILD | WS_VISIBLE, 20, 175, 100, 20, hwnd, 0, hInstance, 0);
        hCmbType = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 120, 172, 110, 100, hwnd, (HMENU)ID_CMB_TYPE, hInstance, 0);
        SendMessageW(hCmbType, CB_ADDSTRING, 0, (LPARAM)L"Single");
        SendMessageW(hCmbType, CB_ADDSTRING, 0, (LPARAM)L"Double");
        SendMessageW(hCmbType, CB_ADDSTRING, 0, (LPARAM)L"Triple");
        SendMessageW(hCmbType, CB_SETCURSEL, 0, 0);

        CreateWindowW(L"STATIC", L"Hotkey:", WS_CHILD | WS_VISIBLE, 20, 220, 60, 20, hwnd, 0, hInstance, 0);
        hTxtHotkey = CreateWindowW(L"STATIC", L"F7", WS_CHILD | WS_VISIBLE | SS_CENTER, 80, 220, 80, 20, hwnd, 0, hInstance, 0);
        hBtnHotkey = CreateWindowW(L"BUTTON", L"Set Hotkey", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170, 215, 80, 30, hwnd, (HMENU)ID_BTN_HOTKEY, hInstance, 0);
        SetWindowSubclass(hBtnHotkey, HotkeyBtnProc, 0, (DWORD_PTR)this);

        hBtnStart = CreateWindowW(L"BUTTON", L"START", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 270, 105, 40, hwnd, (HMENU)ID_BTN_START, hInstance, 0);
        hBtnStop = CreateWindowW(L"BUTTON", L"STOP", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 135, 270, 105, 40, hwnd, (HMENU)ID_BTN_STOP, hInstance, 0);

        EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, font, TRUE);
            return TRUE;
        }, (LPARAM)hMainFont);
    }

    void LoadSettings() {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\kishiClicker", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            auto ReadInt = [&](LPCWSTR name, int def) -> int {
                DWORD val = 0, size = sizeof(DWORD);
                if (RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&val, &size) == ERROR_SUCCESS) return (int)val;
                return def;
            };

            SetWindowTextW(hEditH, std::to_wstring(ReadInt(L"H", 0)).c_str());
            SetWindowTextW(hEditM, std::to_wstring(ReadInt(L"M", 0)).c_str());
            SetWindowTextW(hEditS, std::to_wstring(ReadInt(L"S", 0)).c_str());
            SetWindowTextW(hEditMS, std::to_wstring(ReadInt(L"MS", 100)).c_str());
            SetWindowTextW(hEditRandom, std::to_wstring(ReadInt(L"RandomOffset", 40)).c_str());
            
            int useRandom = ReadInt(L"UseRandom", 1);
            SendMessageW(hChkRandom, BM_SETCHECK, useRandom ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(hEditRandom, useRandom);

            SendMessageW(hCmbBtn, CB_SETCURSEL, ReadInt(L"Btn", 0), 0);
            SendMessageW(hCmbType, CB_SETCURSEL, ReadInt(L"Type", 0), 0);
            
            vkHotkey = ReadInt(L"HotkeyVK", VK_F7);
            modHotkey = ReadInt(L"HotkeyMod", 0);

            RegCloseKey(hKey);
        }

        if (!RegisterHotKey(hwndMain, HOTKEY_ID, modHotkey, vkHotkey)) {
            MessageBoxW(hwndMain, L"Hotkey is occupied. Reset to None.", L"Warning", MB_OK);
            vkHotkey = 0; 
            modHotkey = 0;
            SetWindowTextW(hTxtHotkey, L"None");
        } else {
            SetWindowTextW(hTxtHotkey, GetHotkeyString(modHotkey, vkHotkey).c_str());
        }
    }

    void SaveSettings() {
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\kishiClicker", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            auto WriteInt = [&](LPCWSTR name, int val) {
                DWORD dVal = (DWORD)val;
                RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&dVal, sizeof(DWORD));
            };

            WriteInt(L"H", (int)SafeGetTime(hEditH));
            WriteInt(L"M", (int)SafeGetTime(hEditM));
            WriteInt(L"S", (int)SafeGetTime(hEditS));
            WriteInt(L"MS", (int)SafeGetTime(hEditMS));
            WriteInt(L"RandomOffset", (int)SafeGetTime(hEditRandom));
            WriteInt(L"UseRandom", SendMessageW(hChkRandom, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            WriteInt(L"Btn", (int)SendMessageW(hCmbBtn, CB_GETCURSEL, 0, 0));
            WriteInt(L"Type", (int)SendMessageW(hCmbType, CB_GETCURSEL, 0, 0));
            WriteInt(L"HotkeyVK", vkHotkey);
            WriteInt(L"HotkeyMod", modHotkey);

            RegCloseKey(hKey);
        }
    }

    std::wstring GetHotkeyString(UINT mod, int vk) {
        if (vk == 0) return L"None";
        std::wstring str = L"";
        if (mod & MOD_CONTROL) str += L"Ctrl + ";
        if (mod & MOD_ALT)     str += L"Alt + ";
        if (mod & MOD_SHIFT)   str += L"Shift + ";
        
        if (vk >= 0x70 && vk <= 0x87) {
            str += L"F" + std::to_wstring(vk - 0x6F);
        } else {
            WCHAR keyName[32] = {0};
            LONG lScan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
            if (vk >= VK_PRIOR && vk <= VK_DOWN) lScan |= 0x01000000;
            GetKeyNameTextW(lScan, keyName, 32);
            str += keyName;
        }
        return str;
    }

    void UpdateUIState(bool running) {
        EnableWindow(hBtnStart, !running);
        EnableWindow(hBtnStop, running);
        
        HWND ctrls[] = { hEditH, hEditM, hEditS, hEditMS, hChkRandom, hCmbBtn, hCmbType, hBtnHotkey };
        for (auto h : ctrls) EnableWindow(h, !running);

        if (running) {
            EnableWindow(hEditRandom, FALSE);
        } else {
            BOOL chk = (SendMessageW(hChkRandom, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hEditRandom, chk);
        }
        HICON hCurrentIcon = running ? hIconRun : hIconStop;
        if (hCurrentIcon && hwndMain) {
            SendMessageW(hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)hCurrentIcon);
            SendMessageW(hwndMain, WM_SETICON, ICON_BIG,   (LPARAM)hCurrentIcon);
        }
    }

    int64_t SafeGetTime(HWND hEdit) {
        WCHAR buf[32];
        GetWindowTextW(hEdit, buf, 32);
        return std::max<int64_t>(0, _wtoi(buf));
    }

    void StartAction() {
        int64_t h = SafeGetTime(hEditH);
        int64_t m = SafeGetTime(hEditM);
        int64_t s = SafeGetTime(hEditS);
        int64_t ms = SafeGetTime(hEditMS);
        int randOffset = (int)SafeGetTime(hEditRandom);
        
        bool useRand = (SendMessageW(hChkRandom, BM_GETCHECK, 0, 0) == BST_CHECKED);
        int btn = (int)SendMessageW(hCmbBtn, CB_GETCURSEL, 0, 0);
        int type = (int)SendMessageW(hCmbType, CB_GETCURSEL, 0, 0);

        ClickerConfig config(h, m, s, ms, randOffset, useRand, btn, type);
        engine.Start(config);
        UpdateUIState(true);
    }

    void StopAction() {
        engine.Stop();
        UpdateUIState(false);
    }

    static LRESULT CALLBACK HotkeyBtnProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
        auto app = (AutoClickerApp*)dwRefData;
        if (!app) return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        return app->HandleHotkeyInput(hWnd, uMsg, wParam, lParam);
    }

    LRESULT HandleHotkeyInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_KILLFOCUS && isListeningHotkey) {
            isListeningHotkey = false;
            SetWindowTextW(hTxtHotkey, GetHotkeyString(modHotkey, vkHotkey).c_str());
        }
        else if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && isListeningHotkey) {
            int vkCode = (int)wParam;
            if (vkCode != VK_CONTROL && vkCode != VK_SHIFT && vkCode != VK_MENU) {
                UINT mod = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;

                UnregisterHotKey(hwndMain, HOTKEY_ID);
                if (!RegisterHotKey(hwndMain, HOTKEY_ID, mod, vkCode)) {
                    MessageBoxW(hwndMain, L"Hotkey conflict!", L"Error", MB_ICONWARNING);
                    if (vkHotkey != 0) RegisterHotKey(hwndMain, HOTKEY_ID, modHotkey, vkHotkey);
                } else {
                    vkHotkey = vkCode;
                    modHotkey = mod;
                }
                SetWindowTextW(hTxtHotkey, GetHotkeyString(modHotkey, vkHotkey).c_str());
                isListeningHotkey = false;
                SetFocus(hwndMain);
                return 0; 
            }
        } else if (uMsg == WM_GETDLGCODE && isListeningHotkey) return DLGC_WANTALLKEYS;
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(30, 30, 30));
            return (INT_PTR)hbrDarkBg;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(45, 45, 48));
            return (INT_PTR)hbrDarkEdit;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == ID_BTN_START) { StartAction(); SetFocus(hwnd); }
            else if (id == ID_BTN_STOP) { StopAction(); SetFocus(hwnd); }
            else if (id == ID_BTN_HOTKEY) { isListeningHotkey = true; SetWindowTextW(hTxtHotkey, L"Press key..."); }
            else if (id == ID_CHK_RANDOM && code == BN_CLICKED) {
                BOOL chk = (SendMessageW(hChkRandom, BM_GETCHECK, 0, 0) == BST_CHECKED);
                EnableWindow(hEditRandom, chk);
                if (chk && SafeGetTime(hEditRandom) <= 0) SetWindowTextW(hEditRandom, L"40");
            }
            else if (id == ID_EDIT_RANDOM && code == EN_KILLFOCUS) {
                if (SafeGetTime(hEditRandom) <= 0) {
                    SendMessageW(hChkRandom, BM_SETCHECK, BST_UNCHECKED, 0);
                    EnableWindow(hEditRandom, FALSE);
                }
            }
            break;
        }
        case WM_HOTKEY: {
            if (wParam == HOTKEY_ID) {
                if (engine.IsRunning()) StopAction(); else StartAction();
            }
            break;
        }
        case WM_DESTROY: {
            SaveSettings();
            engine.Stop();
            UnregisterHotKey(hwnd, HOTKEY_ID);
            RemoveWindowSubclass(hBtnHotkey, HotkeyBtnProc, (DWORD_PTR)this);
            PostQuitMessage(0);
            break;
        }
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return 0;
    }
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    TimerResolutionGuard timerGuard;
    AutoClickerApp app(hInstance);
    return app.Run(nCmdShow);
}