// payload.cpp  --  the injected DLL. Reports whether a GUI/inventory screen is open.
//
// How it detects "in inventory" WITHOUT reading JVM memory:
//   While you're playing, Minecraft "grabs" the mouse -- it hides the OS cursor and
//   keeps re-centering it. The moment any screen opens (inventory, chat, pause, a
//   chest, the death screen...) the game releases the cursor so you can click the UI.
//   So: OS cursor visible + game focused  =>  a GUI screen is open.
//
//   Limitation (by design, since this is a generic DLL): this can't tell the
//   *inventory* apart from other screens. Distinguishing the specific screen would
//   require reading Minecraft.currentScreen from the JVM (a JVMTI agent or a Forge mod).
//
// Press END (game or this console focused) to unload the DLL cleanly.

#include <windows.h>
#include <cstdio>

static HMODULE        g_self    = nullptr;
static volatile bool  g_running = true;

struct FindData { DWORD pid; HWND hwnd; };

// Pick the main visible top-level window owned by this (the Minecraft) process.
static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* fd = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == fd->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        char title[256];
        if (GetWindowTextA(hwnd, title, sizeof(title)) > 0) {  // skip untitled helper windows
            fd->hwnd = hwnd;
            return FALSE;  // found it, stop enumerating
        }
    }
    return TRUE;
}

static HWND findGameWindow() {
    FindData fd{ GetCurrentProcessId(), nullptr };
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&fd));
    return fd.hwnd;
}

static bool cursorVisible() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;
    return (ci.flags & CURSOR_SHOWING) != 0;
}

static void run() {
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleTitleA("MC Inventory Probe");

    printf("[probe] attached to javaw (pid %lu)\n", GetCurrentProcessId());
    printf("[probe] heuristic: OS cursor visible while game focused => a GUI/inventory screen is open.\n");
    printf("[probe] press END to unload.\n\n");

    HWND game = nullptr;
    int  last = -99;  // force first print

    while (g_running) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            printf("[probe] END pressed -- unloading.\n");
            break;
        }

        if (!game || !IsWindow(game)) game = findGameWindow();

        int state;
        if (game && GetForegroundWindow() == game)
            state = cursorVisible() ? 1 : 0;   // 1 = GUI open, 0 = in-game
        else
            state = -1;                         // game not focused -> unknown

        if (state != last) {
            if      (state == 1)  printf("[probe] In inventory / GUI screen: YES\n");
            else if (state == 0)  printf("[probe] In inventory / GUI screen: NO  (playing)\n");
            else                  printf("[probe] (game window not focused -- state unknown)\n");
            last = state;
        }
        Sleep(100);
    }

    fflush(stdout);
    if (f) fclose(f);
    FreeConsole();
    FreeLibraryAndExitThread(g_self, 0);  // unmap this DLL and end the thread
}

// Thread entry must be a plain function pointer; a non-capturing lambda qualifies.
static DWORD WINAPI threadMain(LPVOID) { run(); return 0; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);  // we don't care about thread attach/detach
        // Never block in DllMain -- hand off to our own thread immediately.
        HANDLE t = CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
