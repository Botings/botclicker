// payload.cpp  --  JVMTI/JNI agent: a left-click autoclicker that never mines blocks.
//
// Injected via injector.exe into javaw.exe (Minecraft 1.8.9 Forge). On attach it grabs
// the running JVM (JNI_GetCreatedJavaVMs), gets a JVMTI env, and locates a few Minecraft
// fields so it can read what the player is looking at.
//
// Behaviour (only while Minecraft is the foreground window):
//   * A low-level mouse hook SWALLOWS your real left-click and we synthesize all clicks
//     ourselves -- this is what lets us NOT mine a block you're holding LMB on.
//   * While you hold LMB we inject clicks at 15.8 CPS, EXCEPT when your crosshair is on a
//     block (objectMouseOver.typeOfHit == BLOCK) -> we inject nothing, so blocks are never hit.
//   * Air / entities -> clicks go through (attack swings).
//   * A GUI is open (currentScreen != null, e.g. your inventory) -> clicks go through.
//
// Field names are resolved by trying MCP (dev) names first, then SRG (prod) names, so the
// same DLL works in a Gradle runClient session and a shipped Forge client. If neither
// resolves it degrades to "always click" and says so in the console.
//
// Press END to unload cleanly.

#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <cstdio>
#include <cstring>

// ---- globals ---------------------------------------------------------------
static HMODULE       g_self      = nullptr;
static volatile bool g_running   = true;

static JavaVM*       g_jvm       = nullptr;
static jvmtiEnv*     g_jvmti     = nullptr;

static jclass        g_mcClass    = nullptr;  // global ref: net.minecraft.client.Minecraft
static jobject       g_mcInstance = nullptr;  // global ref: the Minecraft singleton
static jfieldID      g_fObjMouseOver = nullptr;
static jfieldID      g_fCurrentScreen = nullptr;
static jclass        g_mopClass   = nullptr;  // global ref: MovingObjectPosition (lazy)
static jfieldID      g_fTypeOfHit = nullptr;  // lazy
static jmethodID     g_midOrdinal = nullptr;  // java.lang.Enum.ordinal()
static bool          g_canDetectBlocks = false;

static volatile HWND g_gameWindow = nullptr;
static volatile bool g_physDown    = false;   // real (non-injected) LMB state
static volatile bool g_enabled     = false;   // armed via J toggle; starts off
static HHOOK         g_mouseHook   = nullptr;
static HANDLE        g_hookThread  = nullptr;
static DWORD         g_hookThreadId = 0;

static const double  CPS        = 15.8;
static const double  PERIOD_MS  = 1000.0 / CPS;  // ~63.29 ms

// ---- console ---------------------------------------------------------------
static void logf(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    fflush(stdout);
}

// ---- window helpers --------------------------------------------------------
struct FindData { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* fd = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == fd->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        char t[256];
        if (GetWindowTextA(hwnd, t, sizeof(t)) > 0) { fd->hwnd = hwnd; return FALSE; }
    }
    return TRUE;
}
static HWND findGameWindow() {
    FindData fd{ GetCurrentProcessId(), nullptr };
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&fd));
    return fd.hwnd;
}
static inline bool gameFocused() {
    HWND g = g_gameWindow;
    return g && GetForegroundWindow() == g;
}

// ---- low-level mouse hook: own the left button while MC is focused ----------
static LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        bool injected = (m->flags & LLMHF_INJECTED) != 0;   // our own SendInput clicks
        // Only seize the left button while armed; otherwise real clicks pass through normally.
        if (!injected && g_enabled && gameFocused()) {
            if (w == WM_LBUTTONDOWN) { g_physDown = true;  return 1; }  // swallow real press
            if (w == WM_LBUTTONUP)   { g_physDown = false; return 1; }  // swallow real release
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
static DWORD WINAPI hookThread(LPVOID) {
    g_hookThreadId = GetCurrentThreadId();
    // hMod = our DLL (it contains mouseProc); also keeps a ref while the hook lives.
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, g_self, 0);
    if (!g_mouseHook) { logf("[ac] WARNING: mouse hook failed (%lu)\n", GetLastError()); return 1; }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { /* pump until WM_QUIT */ }
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    return 0;
}

static void sendClick() {
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

// ---- JVMTI field discovery (tries MCP name, then SRG name) ------------------
static jclass findLoadedClass(JNIEnv* env, const char* sig) {
    jint n = 0; jclass* classes = nullptr;
    if (g_jvmti->GetLoadedClasses(&n, &classes) != JVMTI_ERROR_NONE) return nullptr;
    jclass found = nullptr;
    for (jint i = 0; i < n; i++) {
        char* csig = nullptr;
        if (g_jvmti->GetClassSignature(classes[i], &csig, nullptr) == JVMTI_ERROR_NONE && csig) {
            if (strcmp(csig, sig) == 0) found = static_cast<jclass>(env->NewGlobalRef(classes[i]));
            g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(csig));
        }
        if (found) break;
    }
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes));
    return found;
}
// Search fields declared on `klass`; match MCP or SRG name. Reports static-ness.
static jfieldID findField(jclass klass, const char* mcp, const char* srg, bool* isStatic) {
    jint n = 0; jfieldID* fs = nullptr;
    if (g_jvmti->GetClassFields(klass, &n, &fs) != JVMTI_ERROR_NONE) return nullptr;
    jfieldID res = nullptr;
    for (jint i = 0; i < n; i++) {
        char* name = nullptr; char* sig = nullptr;
        if (g_jvmti->GetFieldName(klass, fs[i], &name, &sig, nullptr) == JVMTI_ERROR_NONE && name) {
            if (strcmp(name, mcp) == 0 || (srg && strcmp(name, srg) == 0)) {
                res = fs[i];
                if (isStatic) { jint mod = 0; g_jvmti->GetFieldModifiers(klass, fs[i], &mod); *isStatic = (mod & 0x0008) != 0; }
            }
        }
        if (name) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
        if (sig)  g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
        if (res) break;
    }
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(fs));
    return res;
}

static bool setupJvm(JNIEnv* env) {
    // jvm.dll is already loaded in javaw; resolve the entrypoint dynamically.
    HMODULE jvmDll = GetModuleHandleA("jvm.dll");
    if (!jvmDll) { logf("[ac] jvm.dll not found in process\n"); return false; }
    typedef jint (JNICALL *GetCreatedFn)(JavaVM**, jsize, jsize*);
    auto getCreated = reinterpret_cast<GetCreatedFn>(GetProcAddress(jvmDll, "JNI_GetCreatedJavaVMs"));
    if (!getCreated) { logf("[ac] JNI_GetCreatedJavaVMs missing\n"); return false; }

    JavaVM* vms[1]; jsize cnt = 0;
    if (getCreated(vms, 1, &cnt) != JNI_OK || cnt < 1) { logf("[ac] no JVM found\n"); return false; }
    g_jvm = vms[0];

    if (g_jvm->GetEnv(reinterpret_cast<void**>(&g_jvmti), JVMTI_VERSION_1_2) != JNI_OK || !g_jvmti) {
        logf("[ac] could not get JVMTI env\n"); return false;
    }

    g_mcClass = findLoadedClass(env, "Lnet/minecraft/client/Minecraft;");
    if (!g_mcClass) { logf("[ac] Minecraft class not loaded yet\n"); return false; }

    bool isStatic = false;
    jfieldID fTheMc = findField(g_mcClass, "theMinecraft", "field_71432_P", &isStatic);
    g_fObjMouseOver  = findField(g_mcClass, "objectMouseOver", "field_71476_x", nullptr);
    g_fCurrentScreen = findField(g_mcClass, "currentScreen",   "field_71462_r", nullptr);

    if (fTheMc && isStatic) {
        jobject mc = env->GetStaticObjectField(g_mcClass, fTheMc);
        if (mc) { g_mcInstance = env->NewGlobalRef(mc); env->DeleteLocalRef(mc); }
    }

    jclass enumC = env->FindClass("java/lang/Enum");  // bootstrap class -> findable from native thread
    if (enumC) { g_midOrdinal = env->GetMethodID(enumC, "ordinal", "()I"); env->DeleteLocalRef(enumC); }
    if (env->ExceptionCheck()) env->ExceptionClear();

    g_canDetectBlocks = (g_mcInstance && g_fObjMouseOver && g_midOrdinal);
    logf("[ac] field resolution: theMinecraft=%s objectMouseOver=%s currentScreen=%s\n",
         g_mcInstance ? "ok" : "MISS", g_fObjMouseOver ? "ok" : "MISS", g_fCurrentScreen ? "ok" : "MISS");
    if (!g_canDetectBlocks)
        logf("[ac] WARNING: block detection unavailable -> will click everywhere (incl. blocks).\n");
    return true;
}

// Is a GUI/screen (inventory, chat, menu...) currently open? Used to ignore the J toggle
// while the player is typing, so 'j' in chat doesn't flip the clicker.
static bool screenOpen(JNIEnv* env) {
    if (!g_canDetectBlocks || !g_fCurrentScreen || !g_mcInstance) return false;
    jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
    bool open = (scr != nullptr);
    if (scr) env->DeleteLocalRef(scr);
    return open;
}

// Returns true if a synthetic click is allowed right now.
static bool shouldClick(JNIEnv* env) {
    if (!g_canDetectBlocks) return true;  // degraded mode

    // A GUI (inventory, chest, ...) is open -> safe to click, no blocks involved.
    if (g_fCurrentScreen) {
        jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
        bool gui = (scr != nullptr);
        if (scr) env->DeleteLocalRef(scr);
        if (gui) return true;
    }

    // In-world: inspect the crosshair target.
    jobject mop = env->GetObjectField(g_mcInstance, g_fObjMouseOver);
    if (!mop) return true;  // nothing targeted (air) -> allow attack swing

    if (!g_mopClass) {  // resolve typeOfHit once, off the live object's class
        jclass c = env->GetObjectClass(mop);
        g_mopClass = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
        g_fTypeOfHit = findField(g_mopClass, "typeOfHit", "field_72313_a", nullptr);
    }

    bool isBlock = false;
    if (g_fTypeOfHit) {
        jobject t = env->GetObjectField(mop, g_fTypeOfHit);  // MovingObjectType enum
        if (t) {
            jint ord = env->CallIntMethod(t, g_midOrdinal);  // MISS=0, BLOCK=1, ENTITY=2
            if (env->ExceptionCheck()) env->ExceptionClear();
            isBlock = (ord == 1);
            env->DeleteLocalRef(t);
        }
    }
    env->DeleteLocalRef(mop);
    return !isBlock;  // skip only when aimed at a block
}

// ---- main agent thread -----------------------------------------------------
static void run() {
    AllocConsole();
    FILE* f = nullptr; freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleTitleA("MC AutoClicker (no-block)");

    logf("[ac] attached to pid %lu. resolving JVM...\n", GetCurrentProcessId());

    JNIEnv* env = nullptr;
    if (g_jvm == nullptr) {
        // Bootstrap: get a JavaVM* first so we can attach this thread.
        HMODULE jvmDll = GetModuleHandleA("jvm.dll");
        typedef jint (JNICALL *GetCreatedFn)(JavaVM**, jsize, jsize*);
        auto getCreated = jvmDll ? reinterpret_cast<GetCreatedFn>(GetProcAddress(jvmDll, "JNI_GetCreatedJavaVMs")) : nullptr;
        JavaVM* vms[1]; jsize cnt = 0;
        if (getCreated && getCreated(vms, 1, &cnt) == JNI_OK && cnt >= 1) g_jvm = vms[0];
    }
    if (!g_jvm || g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
        logf("[ac] FATAL: could not attach to JVM. Is this Minecraft Java?\n");
    } else {
        setupJvm(env);
    }

    logf("[ac] running at %.1f CPS. Press J to toggle (starts OFF). Hold LEFT-CLICK while armed. Press END to unload.\n\n", CPS);

    g_hookThread = CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);

    int  lastState = -99;   // 0 idle, 1 clicking, 2 holding-on-block, 3 degraded-click
    double acc = 0.0;
    bool lastJ = false;

    while (g_running) {
        if (GetAsyncKeyState(VK_END) & 0x8000) { logf("[ac] END pressed -- unloading.\n"); break; }

        g_gameWindow = findGameWindow();

        // J toggle: edge-detected, only when focused and not typing in a screen/chat.
        bool jDown = (GetAsyncKeyState('J') & 0x8000) != 0;
        if (jDown && !lastJ && gameFocused() && !(env && screenOpen(env))) {
            g_enabled = !g_enabled;
            if (!g_enabled) g_physDown = false;          // drop any held state when disarming
            logf("[ac] %s\n", g_enabled ? "ENABLED (J)" : "DISABLED (J)");
            lastState = -99;                              // force activity line to reprint
        }
        lastJ = jDown;

        int state = 0;
        if (g_enabled && g_physDown && gameFocused()) {
            bool ok = env ? shouldClick(env) : true;
            if (ok) { sendClick(); state = g_canDetectBlocks ? 1 : 3; }
            else    { state = 2; }  // aimed at a block -> deliberately doing nothing
        } else if (!gameFocused()) {
            g_physDown = false;     // avoid a stuck "down" if focus was lost mid-hold
        }

        if (g_enabled && state != lastState) {
            if      (state == 1) logf("[ac] autoclicking (entity/air)\n");
            else if (state == 2) logf("[ac] aimed at block -> NOT clicking\n");
            else if (state == 3) logf("[ac] autoclicking (no block-detect)\n");
            else                 logf("[ac] armed, idle\n");
            lastState = state;
        }

        acc += PERIOD_MS;
        DWORD s = static_cast<DWORD>(acc);
        acc -= s; if (s < 1) s = 1;
        Sleep(s);
    }

    g_running = false;

    // Tear the hook thread down BEFORE unloading: otherwise the installed hook proc
    // would point into freed DLL memory and crash the game on the next mouse event.
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) { WaitForSingleObject(g_hookThread, 3000); CloseHandle(g_hookThread); }

    if (env && g_jvm) g_jvm->DetachCurrentThread();

    fflush(stdout); if (f) fclose(f); FreeConsole();
    FreeLibraryAndExitThread(g_self, 0);
}

static DWORD WINAPI threadMain(LPVOID) { run(); return 0; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
