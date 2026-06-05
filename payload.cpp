// payload.cpp  --  JVMTI/JNI agent: a left-click autoclicker that never mines blocks.
//
// Injected via injector.exe into javaw.exe (Minecraft 1.8.9 Forge). On attach it grabs
// the running JVM (JNI_GetCreatedJavaVMs), gets a JVMTI env, and locates a few Minecraft
// fields so it can read what the player is looking at.
//
// UI: instead of a text console, the agent opens "BotClicker" -- a small frameless,
// always-on-top GDI+ window (dark space-blue theme). Each feature is a toggle button that
// glows GREEN when on and RED when off. Drag it by the title bar; the little X (or END)
// unloads cleanly.
//
// Toggles:
//   * Autoclicker   -- master arm/disarm (also bound to the J key). Off by default.
//   * Block Guard   -- when aimed at a block, inject nothing so blocks are never mined.
//   * Back-Turn Slow-- drop to a low CPS when the targeted player's back is turned.
//   * Inv. Throttle -- in a GUI with a full hotbar, space clicks past the double-click window.
//   * Splash Pot Move-- in a container GUI, shift-click any splash potion you hover (once per
//     slot until the inventory is reopened). Runs on its own fast thread, not the CPS pacing.
//
// Behaviour (only while Minecraft is the foreground window):
//   * A low-level mouse hook SWALLOWS your real left-click and we synthesize all clicks
//     ourselves -- this is what lets us NOT mine a block you're holding LMB on.
//   * While armed and holding LMB we inject clicks at the slider-set rate (drag 0-30 CPS,
//     default 15.8). Block Guard / Back-Turn / Inv. Throttle each gate or pace those clicks.
//
// Field names are resolved by trying MCP (dev) names first, then SRG (prod) names, so the
// same DLL works in a Gradle runClient session and a shipped Forge client. If neither
// resolves it degrades to "always click" and says so in the status line.

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <jni.h>
#include <jvmti.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <chrono>
#include <thread>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod (link -lwinmm)

#pragma comment(lib, "gdiplus.lib")

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
static jfieldID      g_fEntityHit = nullptr;  // lazy: MovingObjectPosition.entityHit
static jmethodID     g_midOrdinal = nullptr;  // java.lang.Enum.ordinal()
static bool          g_canDetectBlocks = false;

// For the "target's back is turned" combat heuristic (read other entity's facing).
static jfieldID      g_fThePlayer = nullptr;  // Minecraft.thePlayer
static jclass        g_entityClass = nullptr; // global ref: net.minecraft.entity.Entity
static jfieldID      g_fPosX = nullptr, g_fPosZ = nullptr, g_fYaw = nullptr;
static bool          g_canDetectFacing = false;

// For "is the hotbar full" -> EntityPlayer.inventory.mainInventory[0..8].
static jfieldID      g_fInventory = nullptr; // EntityPlayer.inventory
static jfieldID      g_fMainInv   = nullptr; // InventoryPlayer.mainInventory (ItemStack[])
static bool          g_canDetectHotbar = false;

// For "splash-pot shift-click": when hovering a splash potion in a container GUI, shift-click it
// once, then blacklist that slot until the inventory is reopened. Resolved lazily (the GUI/Slot
// classes may not be loaded until the first container is opened). Runs on its own fast thread.
static jclass        g_guiContainerClass = nullptr; // global ref: GuiContainer
static jfieldID      g_fTheSlot   = nullptr;        // GuiContainer.theSlot (hovered Slot)
static jfieldID      g_fSlotNumber= nullptr;        // Slot.slotNumber
static jmethodID     g_midGetStack= nullptr;        // Slot.getStack()
static jmethodID     g_midGetItem = nullptr;        // ItemStack.getItem()
static jmethodID     g_midGetMeta = nullptr;        // ItemStack.getMetadata()
static jclass        g_itemPotionClass = nullptr;   // global ref: ItemPotion
static bool          g_canPotMove = false;          // all of the above resolved
static jobject       g_lastInvScreen = nullptr;     // global ref: detect inventory reopen (pot thread only)
static bool          g_potDone[256] = {};           // slot indices already shift-clicked this open

static volatile HWND g_gameWindow = nullptr;
static volatile bool g_physDown      = false; // real (non-injected) LMB state
static volatile bool g_physRightDown = false; // real (non-injected) RMB state
static HHOOK         g_mouseHook   = nullptr;
static HANDLE        g_hookThread  = nullptr;
static DWORD         g_hookThreadId = 0;

// ---- toggleable options (shared with the GUI) ------------------------------
static volatile bool g_enabled     = false;   // master arm; starts off
static volatile bool g_rightClicker= false;   // also rapid right-click while holding RMB
static volatile bool g_blockGuard  = true;    // don't mine blocks
static volatile bool g_backTurn    = true;    // slow CPS on a back-turned target
static volatile bool g_invThrottle = true;    // space inventory clicks when hotbar full
static volatile bool g_potMove     = false;   // shift-click splash pots you hover in a GUI

// Master-toggle hotkey. Rebindable from the GUI: click the row, then press any key.
static volatile int  g_hotkey      = 'J';
static wchar_t       g_hotkeyName[32] = L"J";
static volatile bool g_rebinding   = false;   // currently capturing a new key

// Click rate, adjustable live from the GUI slider (drag 0..30 CPS). 0 = effectively paused.
static volatile double g_cps     = 15.8;
static const double  CPS_MIN     = 0.0;
static const double  CPS_MAX     = 30.0;
static const double  CPS_OFF_EPS = 0.1;          // below this we treat the clicker as paused
// Vanilla treats two clicks on the same slot within 250 ms as a double-click. When the hotbar
// is full we space inventory clicks beyond this so they can't gather a stack.
static const ULONGLONG INVENTORY_MIN_MS = 300;
// When the targeted player's back is turned, drop the click rate to this.
static const double    BACKTURN_CPS   = 6.0;
static const double    BACKTURN_ANGLE = 90.0;  // degrees away from "facing me" = back turned
static const double    PI             = 3.14159265358979323846;

// In-world crosshair evaluation results.
enum { EVAL_ALLOW = 0, EVAL_BLOCK = 1, EVAL_BACKTURN = 2 };

// ---- log / status ----------------------------------------------------------
// No console any more. Diagnostics go to the debugger (DebugView), and a friendly
// one-liner is mirrored into the GUI status pill.
static char g_status[96] = "starting...";
static char g_diag[96]   = "";
static HWND g_hwnd = nullptr;

static void requestRepaint() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

static void logf(const char* fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    OutputDebugStringA(buf);
}
static void setStatus(const char* s) {
    if (strncmp(g_status, s, sizeof(g_status)) != 0) {
        strncpy_s(g_status, s, _TRUNCATE);
        requestRepaint();
    }
}

// Friendly label for a virtual-key code (e.g. 'J' -> "J", VK_SPACE -> "Space").
static void computeHotkeyName(int vk) {
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {  // extended-key bit -> correct names for these
    case VK_LEFT: case VK_RIGHT: case VK_UP:   case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END:  case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= (1L << 24); break;
    }
    wchar_t buf[32] = {};
    if (sc != 0 && GetKeyNameTextW(lp, buf, 31) > 0) wcsncpy_s(g_hotkeyName, buf, _TRUNCATE);
    else                                             swprintf_s(g_hotkeyName, L"0x%02X", (unsigned)vk);
}

// ---- window helpers --------------------------------------------------------
struct FindData { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* fd = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == fd->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        char t[256];
        // Skip our own BotClicker window so it never gets mistaken for the game window.
        if (hwnd != g_hwnd && GetWindowTextA(hwnd, t, sizeof(t)) > 0) { fd->hwnd = hwnd; return FALSE; }
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
    HWND fg = GetForegroundWindow();
    // Treat our own overlay as "the game has focus" so clicking a toggle doesn't disarm us.
    return g && (fg == g || fg == g_hwnd);
}

// ---- low-level mouse hook: own the left button while MC is focused ----------
static LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        bool injected = (m->flags & LLMHF_INJECTED) != 0;   // our own SendInput clicks
        // Only seize buttons while armed AND the game (not our overlay) is focused.
        if (!injected && g_enabled && g_gameWindow && GetForegroundWindow() == g_gameWindow) {
            if (w == WM_LBUTTONDOWN) { g_physDown = true;  return 1; }  // swallow real press
            if (w == WM_LBUTTONUP)   { g_physDown = false; return 1; }  // swallow real release
            // Right button is only seized when the Right Clicker is enabled.
            if (g_rightClicker) {
                if (w == WM_RBUTTONDOWN) { g_physRightDown = true;  return 1; }
                if (w == WM_RBUTTONUP)   { g_physRightDown = false; return 1; }
            }
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

static void sendRightClick() {
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(2, in, sizeof(INPUT));
}

// Shift + left-click of the hovered slot -> the game's quick-move (shift-click). LWJGL polls
// keyboard state rather than reading it per-event, so we hold shift across the click with small
// gaps (>= one client tick) instead of one atomic batch, or it may miss that shift was down.
static void sendShiftClick() {
    INPUT k = {}; k.type = INPUT_KEYBOARD; k.ki.wVk = VK_LSHIFT;
    SendInput(1, &k, sizeof(INPUT));                 // shift down
    Sleep(25);
    INPUT m[2] = {};
    m[0].type = INPUT_MOUSE; m[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    m[1].type = INPUT_MOUSE; m[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, m, sizeof(INPUT));                  // click while shift held
    Sleep(25);
    k.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &k, sizeof(INPUT));                 // shift up
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

// Resolve a method by MCP name, then SRG name. Clears the pending NoSuchMethodError on a miss.
static jmethodID findMethod(JNIEnv* env, jclass klass, const char* mcp, const char* srg, const char* sig) {
    jmethodID m = env->GetMethodID(klass, mcp, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!m && srg) {
        m = env->GetMethodID(klass, srg, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return m;
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

    // Entity facing fields: Minecraft.thePlayer + Entity.{posX,posZ,rotationYaw}. Field IDs taken
    // from the Entity base class are valid for any subclass instance (the player and the target).
    g_fThePlayer  = findField(g_mcClass, "thePlayer", "field_71439_g", nullptr);
    g_entityClass = findLoadedClass(env, "Lnet/minecraft/entity/Entity;");
    if (g_entityClass) {
        g_fPosX = findField(g_entityClass, "posX",        "field_70165_t", nullptr);
        g_fPosZ = findField(g_entityClass, "posZ",        "field_70161_v", nullptr);
        g_fYaw  = findField(g_entityClass, "rotationYaw", "field_70177_z", nullptr);
    }
    g_canDetectFacing = g_fThePlayer && g_entityClass && g_fPosX && g_fPosZ && g_fYaw;
    logf("[ac] facing detection: %s (back-turned target -> %.0f CPS)\n",
         g_canDetectFacing ? "ok" : "MISS -> full CPS on all targets", BACKTURN_CPS);

    // Hotbar-full detection: EntityPlayer.inventory + InventoryPlayer.mainInventory. Field IDs
    // stay valid for the class lifetime, so we don't need to keep the class refs around.
    jclass playerClass = findLoadedClass(env, "Lnet/minecraft/entity/player/EntityPlayer;");
    jclass invClass    = findLoadedClass(env, "Lnet/minecraft/entity/player/InventoryPlayer;");
    if (playerClass) { g_fInventory = findField(playerClass, "inventory", "field_71071_by", nullptr); env->DeleteGlobalRef(playerClass); }
    if (invClass)    { g_fMainInv   = findField(invClass,    "mainInventory", "field_70462_a", nullptr); env->DeleteGlobalRef(invClass); }
    g_canDetectHotbar = g_fThePlayer && g_fInventory && g_fMainInv;
    logf("[ac] hotbar detection: %s\n", g_canDetectHotbar ? "ok" : "MISS -> no inventory throttle");

    _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE, "blocks %s  \xc2\xb7  facing %s  \xc2\xb7  hotbar %s",
                g_canDetectBlocks ? "ok" : "off", g_canDetectFacing ? "ok" : "off",
                g_canDetectHotbar ? "ok" : "off");
    requestRepaint();
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

// Are all 9 hotbar slots (mainInventory[0..8]) occupied? In 1.8.9 an empty slot is null.
// If we can't read the inventory, report "not full" so we don't throttle unexpectedly.
static bool hotbarFull(JNIEnv* env) {
    if (!g_canDetectHotbar) return false;
    jobject me = env->GetObjectField(g_mcInstance, g_fThePlayer);
    if (!me) return false;
    jobject inv = env->GetObjectField(me, g_fInventory);
    env->DeleteLocalRef(me);
    if (!inv) return false;
    jobjectArray main = static_cast<jobjectArray>(env->GetObjectField(inv, g_fMainInv));
    env->DeleteLocalRef(inv);
    if (!main) return false;

    bool full = true;
    for (jint i = 0; i < 9; i++) {
        jobject slot = env->GetObjectArrayElement(main, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); full = false; if (slot) env->DeleteLocalRef(slot); break; }
        if (!slot) { full = false; break; }   // empty slot -> not full
        env->DeleteLocalRef(slot);
    }
    env->DeleteLocalRef(main);
    return full;
}

// True if the targeted entity's back is turned to us (it is facing away). Compares the
// target's yaw against the yaw it WOULD need to face us; >90 deg apart = back hemisphere.
static bool targetBackTurned(JNIEnv* env, jobject mop) {
    if (!g_canDetectFacing || !g_fEntityHit) return false;
    jobject target = env->GetObjectField(mop, g_fEntityHit);
    jobject me     = env->GetObjectField(g_mcInstance, g_fThePlayer);
    bool backTurned = false;
    if (target && me) {
        double tx = env->GetDoubleField(target, g_fPosX);
        double tz = env->GetDoubleField(target, g_fPosZ);
        double mx = env->GetDoubleField(me,     g_fPosX);
        double mz = env->GetDoubleField(me,     g_fPosZ);
        float  tyaw = env->GetFloatField(target, g_fYaw);

        // Minecraft look vector for yaw y: (-sin y, cos y). Yaw to face from target toward me:
        double dx = mx - tx, dz = mz - tz;
        double yawToMe = std::atan2(-dx, dz) * 180.0 / PI;
        double diff = std::fmod((double) tyaw - yawToMe, 360.0);
        if (diff < -180.0) diff += 360.0;
        if (diff >  180.0) diff -= 360.0;
        backTurned = std::fabs(diff) > BACKTURN_ANGLE;
    }
    if (target) env->DeleteLocalRef(target);
    if (me)     env->DeleteLocalRef(me);
    return backTurned;
}

// Evaluate the in-world crosshair target. Caller handles the GUI case first.
//   EVAL_ALLOW    -> click at full CPS (air, or an entity facing us)
//   EVAL_BLOCK    -> aimed at a block, do not click
//   EVAL_BACKTURN -> entity with its back turned, click at reduced CPS
static int evalTarget(JNIEnv* env) {
    if (!g_canDetectBlocks) return EVAL_ALLOW;  // degraded mode

    jobject mop = env->GetObjectField(g_mcInstance, g_fObjMouseOver);
    if (!mop) return EVAL_ALLOW;  // nothing targeted (air) -> allow attack swing

    if (!g_mopClass) {  // resolve typeOfHit + entityHit once, off the live object's class
        jclass c = env->GetObjectClass(mop);
        g_mopClass = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
        g_fTypeOfHit = findField(g_mopClass, "typeOfHit", "field_72313_a", nullptr);
        g_fEntityHit = findField(g_mopClass, "entityHit", "field_72308_g", nullptr);
    }

    int result = EVAL_ALLOW;
    if (g_fTypeOfHit) {
        jobject t = env->GetObjectField(mop, g_fTypeOfHit);  // MovingObjectType enum
        if (t) {
            jint ord = env->CallIntMethod(t, g_midOrdinal);  // MISS=0, BLOCK=1, ENTITY=2
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(t);
            if (ord == 1) result = EVAL_BLOCK;
            else if (ord == 2 && targetBackTurned(env, mop)) result = EVAL_BACKTURN;
        }
    }
    env->DeleteLocalRef(mop);
    return result;
}

// Lazily resolve the GuiContainer / Slot / ItemStack / ItemPotion handles needed for the
// splash-pot shift-click. Returns true once everything is found; keeps retrying until then
// (the GUI/Slot classes load only after the first container is opened).
static bool ensurePotRefs(JNIEnv* env) {
    if (g_canPotMove) return true;
    if (!g_guiContainerClass)
        g_guiContainerClass = findLoadedClass(env, "Lnet/minecraft/client/gui/inventory/GuiContainer;");
    if (!g_guiContainerClass) return false;  // no container GUI loaded yet
    if (!g_fTheSlot) g_fTheSlot = findField(g_guiContainerClass, "theSlot", "field_147006_u", nullptr);

    jclass slotC = findLoadedClass(env, "Lnet/minecraft/inventory/Slot;");
    if (slotC) {
        if (!g_fSlotNumber) g_fSlotNumber = findField(slotC, "slotNumber", "field_75222_d", nullptr);
        if (!g_midGetStack) g_midGetStack = findMethod(env, slotC, "getStack", "func_75211_c",
                                                       "()Lnet/minecraft/item/ItemStack;");
        env->DeleteGlobalRef(slotC);
    }
    jclass stackC = findLoadedClass(env, "Lnet/minecraft/item/ItemStack;");
    if (stackC) {
        if (!g_midGetItem) g_midGetItem = findMethod(env, stackC, "getItem", "func_77973_b",
                                                     "()Lnet/minecraft/item/Item;");
        if (!g_midGetMeta) g_midGetMeta = findMethod(env, stackC, "getMetadata", "func_77960_j", "()I");
        env->DeleteGlobalRef(stackC);
    }
    if (!g_itemPotionClass)
        g_itemPotionClass = findLoadedClass(env, "Lnet/minecraft/item/ItemPotion;");

    g_canPotMove = g_fTheSlot && g_fSlotNumber && g_midGetStack &&
                   g_midGetItem && g_midGetMeta && g_itemPotionClass;
    if (g_canPotMove) logf("[ac] pot shift-click: handles resolved\n");
    return g_canPotMove;
}

// Dedicated fast thread (~tick-rate, NOT gated by the CPS pacing): while a container GUI is
// open and the hovered slot holds a splash potion, shift-click it once, then blacklist that
// slot index until the inventory is closed/reopened.
static DWORD WINAPI potThread(LPVOID) {
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK || !env) {
        logf("[ac] pot thread: could not attach to JVM\n");
        return 0;
    }

    // 1 ms tick via a high-resolution clock (process-wide timer resolution is raised in run()).
    using clock = std::chrono::high_resolution_clock;
    auto nextTick = clock::now();

    while (g_running) {
        bool active = g_potMove && g_mcInstance && g_fCurrentScreen &&
                      g_gameWindow && GetForegroundWindow() == g_gameWindow;
        if (active) {
            jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
            if (scr) {
                if (ensurePotRefs(env) && env->IsInstanceOf(scr, g_guiContainerClass)) {
                    // Inventory (re)opened? -> clear the blacklist for the new container.
                    if (!g_lastInvScreen || !env->IsSameObject(scr, g_lastInvScreen)) {
                        if (g_lastInvScreen) env->DeleteGlobalRef(g_lastInvScreen);
                        g_lastInvScreen = env->NewGlobalRef(scr);
                        memset(g_potDone, 0, sizeof(g_potDone));
                    }
                    jobject slot = env->GetObjectField(scr, g_fTheSlot);  // hovered slot, or null
                    if (slot) {
                        jint sn = env->GetIntField(slot, g_fSlotNumber);
                        bool ignored = (sn >= 0 && sn < 256 && g_potDone[sn]);
                        if (!ignored) {
                            jobject stack = env->CallObjectMethod(slot, g_midGetStack);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (stack) {
                                jobject item = env->CallObjectMethod(stack, g_midGetItem);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                jint meta = env->CallIntMethod(stack, g_midGetMeta);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                // 1.8.9: a potion is a splash when bit 0x4000 is set in its metadata.
                                bool splash = item && env->IsInstanceOf(item, g_itemPotionClass) &&
                                              (meta & 0x4000) != 0;
                                if (splash) {
                                    sendShiftClick();
                                    if (sn >= 0 && sn < 256) g_potDone[sn] = true;
                                    logf("[ac] pot shift-click slot %d (meta 0x%X)\n", (int)sn, (unsigned)meta);
                                }
                                if (item) env->DeleteLocalRef(item);
                                env->DeleteLocalRef(stack);
                            }
                        }
                        env->DeleteLocalRef(slot);
                    }
                }
                env->DeleteLocalRef(scr);
            } else if (g_lastInvScreen) {
                // No screen open -> inventory closed; drop the blacklist + tracked screen.
                env->DeleteGlobalRef(g_lastInvScreen); g_lastInvScreen = nullptr;
                memset(g_potDone, 0, sizeof(g_potDone));
            }
        }
        nextTick += std::chrono::milliseconds(1);   // independent of the click-speed slider
        auto tnow = clock::now();
        if (nextTick < tnow) nextTick = tnow;        // fell behind (e.g. after a shift-click) -> don't spiral
        std::this_thread::sleep_until(nextTick);
    }

    if (g_lastInvScreen) { env->DeleteGlobalRef(g_lastInvScreen); g_lastInvScreen = nullptr; }
    if (g_jvm) g_jvm->DetachCurrentThread();
    return 0;
}

// ============================================================================
//  BotClicker GUI  --  frameless GDI+ window, dark space-blue, red/green toggles
// ============================================================================
using namespace Gdiplus;

static const int   WIN_W = 360, WIN_H = 718;
static const int   TOG_X = 22, TOG_W = WIN_W - 44, TOG_H = 54, TOG_GAP = 12, TOG_Y0 = 120;
static const int   N_TOGGLES = 6;

struct ToggleDef { const wchar_t* label; const wchar_t* hint; volatile bool* state; };
static ToggleDef g_toggles[N_TOGGLES] = {
    { L"Autoclicker",    L"left-click (hotkey)", &g_enabled     },
    { L"Right Clicker",  L"rapid RMB use",       &g_rightClicker},
    { L"Block Guard",    L"never mine blocks",   &g_blockGuard  },
    { L"Back-Turn Slow", L"ease off back-stabs", &g_backTurn    },
    { L"Inv. Throttle",  L"no stack double-tap", &g_invThrottle },
    { L"Splash Pot Move",L"shift-click pots in GUI", &g_potMove  },
};
static RECT g_togRc[N_TOGGLES];
static RECT g_sliderRc;              // the "Click Speed" slider row
static bool g_dragSlider = false;    // currently dragging the slider knob
static RECT g_keyRc;                 // the "Toggle Hotkey" row
static RECT g_closeRc = { WIN_W - 40, 14, WIN_W - 16, 38 };
static int  g_hover = -1;            // hovered toggle index, or -1
static bool g_hoverClose = false;
static bool g_hoverKey   = false;
static ULONG_PTR g_gdipToken = 0;

static void layoutToggles() {
    for (int i = 0; i < N_TOGGLES; i++) {
        int top = TOG_Y0 + i * (TOG_H + TOG_GAP);
        g_togRc[i] = { TOG_X, top, TOG_X + TOG_W, top + TOG_H };
    }
    int st = TOG_Y0 + N_TOGGLES * (TOG_H + TOG_GAP);   // slider row, under the toggles
    g_sliderRc = { TOG_X, st, TOG_X + TOG_W, st + TOG_H };
    int kt = st + (TOG_H + TOG_GAP);                    // hotkey row, under the slider
    g_keyRc = { TOG_X, kt, TOG_X + TOG_W, kt + TOG_H };
}
static inline bool inRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---- slider geometry / value mapping ---------------------------------------
static void sliderTrack(double* x0, double* x1, double* y) {
    *x0 = g_sliderRc.left + 24;
    *x1 = g_sliderRc.right - 24;
    *y  = g_sliderRc.top + 36;
}
static double cpsToX(double x0, double x1) {
    double f = (g_cps - CPS_MIN) / (CPS_MAX - CPS_MIN);
    if (f < 0) f = 0; if (f > 1) f = 1;
    return x0 + f * (x1 - x0);
}
static void setCpsFromX(double px, double x0, double x1) {
    double f = (px - x0) / (x1 - x0);
    if (f < 0) f = 0; if (f > 1) f = 1;
    double v = CPS_MIN + f * (CPS_MAX - CPS_MIN);
    g_cps = floor(v * 10.0 + 0.5) / 10.0;   // snap to 0.1 CPS
}

static Color C(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color(a, r, g, b); }

static void fillRound(Graphics& g, const Color& col, RectF r, float rad) {
    GraphicsPath p;
    float d = rad * 2;
    p.AddArc(r.X,               r.Y,                d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y,                d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d,   0, 90);
    p.AddArc(r.X,               r.Y + r.Height - d, d, d,  90, 90);
    p.CloseFigure();
    SolidBrush b(col);
    g.FillPath(&b, &p);
}
static void strokeRound(Graphics& g, const Color& col, float w, RectF r, float rad) {
    GraphicsPath p;
    float d = rad * 2;
    p.AddArc(r.X,               r.Y,                d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y,                d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d,   0, 90);
    p.AddArc(r.X,               r.Y + r.Height - d, d, d,  90, 90);
    p.CloseFigure();
    Pen pen(col, w);
    g.DrawPath(&pen, &p);
}

// Lazily-built process-lifetime fonts (created after GdiplusStartup).
static Font *fTitle=nullptr,*fSub=nullptr,*fLabel=nullptr,*fHint=nullptr,*fPill=nullptr,*fStatus=nullptr,*fTiny=nullptr;
static void ensureFonts() {
    if (fTitle) return;
    static FontFamily ff(L"Segoe UI");
    fTitle  = new Font(&ff, 27, FontStyleBold,    UnitPixel);
    fSub    = new Font(&ff, 11, FontStyleRegular, UnitPixel);
    fLabel  = new Font(&ff, 15, FontStyleBold,    UnitPixel);
    fHint   = new Font(&ff, 10, FontStyleRegular, UnitPixel);
    fPill   = new Font(&ff, 11, FontStyleBold,    UnitPixel);
    fStatus = new Font(&ff, 12, FontStyleRegular, UnitPixel);
    fTiny   = new Font(&ff, 10, FontStyleRegular, UnitPixel);
}

static void wideOf(const char* s, wchar_t* out, int cch) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cch);
}

static void paint(HWND hwnd, HDC hdc) {
    int W = WIN_W, H = WIN_H;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldb = (HBITMAP)SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        ensureFonts();

        // ---- space-blue backdrop (vertical gradient) + soft border ----
        RectF full(0, 0, (REAL)W, (REAL)H);
        LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)H), C(15, 21, 40), C(7, 10, 22));
        g.FillRectangle(&bg, full);
        // faint accent glow behind the title
        {
            GraphicsPath gp; gp.AddEllipse(-60.0f, -120.0f, (REAL)W + 120, 240.0f);
            PathGradientBrush glow(&gp);
            Color ctr = C(40, 90, 200, 70); glow.SetCenterColor(ctr);
            Color edge = C(40, 90, 200, 0); int n = 1; glow.SetSurroundColors(&edge, &n);
            g.FillPath(&glow, &gp);
        }
        strokeRound(g, C(46, 60, 100), 1.4f, RectF(0.7f, 0.7f, W - 1.4f, H - 1.4f), 17.0f);

        StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
        StringFormat sfL; sfL.SetAlignment(StringAlignmentNear);   sfL.SetLineAlignment(StringAlignmentCenter);

        // ---- title: "Bot" (accent) + "Clicker" (white) ----
        {
            RectF mBot, mClk;
            g.MeasureString(L"Bot", -1, fTitle, PointF(0, 0), &mBot);
            g.MeasureString(L"Clicker", -1, fTitle, PointF(0, 0), &mClk);
            REAL total = mBot.Width + mClk.Width;
            REAL x = (W - total) / 2.0f, y = 30.0f;
            SolidBrush accent(C(96, 165, 255)), white(C(234, 240, 252));
            g.DrawString(L"Bot", -1, fTitle, PointF(x, y), &accent);
            g.DrawString(L"Clicker", -1, fTitle, PointF(x + mBot.Width - 2, y), &white);
        }
        // subtitle
        {
            SolidBrush muted(C(120, 135, 170));
            wchar_t sub[48];
            if (g_cps < CPS_OFF_EPS) swprintf_s(sub, L"Hold LMB  \xb7  paused");
            else                     swprintf_s(sub, L"Hold LMB  \xb7  %.1f CPS", g_cps);
            g.DrawString(sub, -1, fSub, RectF(0, 74, (REAL)W, 18), &sfC, &muted);
        }
        // divider
        {
            Pen ln(C(38, 50, 84), 1.0f);
            g.DrawLine(&ln, 28.0f, 104.0f, (REAL)W - 28, 104.0f);
        }

        // ---- close button (top-right) ----
        {
            RectF cr((REAL)g_closeRc.left, (REAL)g_closeRc.top,
                     (REAL)(g_closeRc.right - g_closeRc.left), (REAL)(g_closeRc.bottom - g_closeRc.top));
            fillRound(g, g_hoverClose ? C(210, 64, 64) : C(34, 44, 72), cr, 7.0f);
            Pen xp(g_hoverClose ? C(255, 235, 235) : C(150, 165, 200), 1.8f);
            REAL pad = 7;
            g.DrawLine(&xp, cr.X + pad, cr.Y + pad, cr.X + cr.Width - pad, cr.Y + cr.Height - pad);
            g.DrawLine(&xp, cr.X + cr.Width - pad, cr.Y + pad, cr.X + pad, cr.Y + cr.Height - pad);
        }

        // ---- toggle buttons ----
        wchar_t wbuf[128];
        for (int i = 0; i < N_TOGGLES; i++) {
            const RECT& rc = g_togRc[i];
            bool on = *g_toggles[i].state;
            RectF card((REAL)rc.left, (REAL)rc.top, (REAL)TOG_W, (REAL)TOG_H);

            // card
            Color cardCol = (i == g_hover) ? C(30, 41, 70) : C(21, 29, 52);
            fillRound(g, cardCol, card, 13.0f);
            strokeRound(g, on ? C(46, 120, 80) : C(40, 52, 86), 1.2f, card, 13.0f);

            // left accent strip (state colour)
            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, on ? C(54, 211, 130) : C(228, 78, 78), strip, 2.0f);
            }

            // label + hint
            SolidBrush txt(C(228, 235, 250)), hint(C(118, 132, 168));
            g.DrawString(g_toggles[i].label, -1, fLabel,
                         RectF(card.X + 24, card.Y + 9, TOG_W - 120, 20), &sfL, &txt);
            g.DrawString(g_toggles[i].hint, -1, fHint,
                         RectF(card.X + 24, card.Y + 30, TOG_W - 120, 16), &sfL, &hint);

            // ON/OFF pill (red / green)
            {
                REAL pw = 66, ph = 30;
                RectF pill(card.X + card.Width - pw - 14, card.Y + (TOG_H - ph) / 2, pw, ph);
                Color a = on ? C(46, 204, 113) : C(231, 76, 60);
                Color b = on ? C(33, 168, 92)  : C(192, 57, 43);
                LinearGradientBrush pg(PointF(pill.X, pill.Y), PointF(pill.X, pill.Y + ph), a, b);
                GraphicsPath pp; float d = ph;
                pp.AddArc(pill.X, pill.Y, d, d, 90, 180);
                pp.AddArc(pill.X + pill.Width - d, pill.Y, d, d, 270, 180);
                pp.CloseFigure();
                g.FillPath(&pg, &pp);
                SolidBrush pillTxt(C(255, 255, 255));
                g.DrawString(on ? L"ON" : L"OFF", -1, fPill, pill, &sfC, &pillTxt);
            }
        }

        // ---- "Click Speed" slider row ----
        {
            RectF card((REAL)g_sliderRc.left, (REAL)g_sliderRc.top, (REAL)TOG_W, (REAL)TOG_H);
            fillRound(g, g_dragSlider ? C(30, 41, 70) : C(21, 29, 52), card, 13.0f);
            strokeRound(g, g_dragSlider ? C(96, 165, 255) : C(40, 52, 86), 1.2f, card, 13.0f);

            // accent strip
            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, C(96, 165, 255), strip, 2.0f);
            }

            // label (left) + live value (right)
            SolidBrush txt(C(228, 235, 250));
            g.DrawString(L"Click Speed", -1, fLabel,
                         RectF(card.X + 24, card.Y + 6, TOG_W - 120, 20), &sfL, &txt);
            {
                wchar_t vb[32];
                if (g_cps < CPS_OFF_EPS) swprintf_s(vb, L"off");
                else                     swprintf_s(vb, L"%.1f CPS", g_cps);
                StringFormat sfR; sfR.SetAlignment(StringAlignmentFar); sfR.SetLineAlignment(StringAlignmentCenter);
                SolidBrush vcol(C(96, 165, 255));
                g.DrawString(vb, -1, fPill, RectF(card.X, card.Y + 6, card.Width - 16, 20), &sfR, &vcol);
            }

            // track (background + filled portion)
            double tx0, tx1, ty; sliderTrack(&tx0, &tx1, &ty);
            REAL trH = 5.0f;
            fillRound(g, C(40, 52, 86), RectF((REAL)tx0, (REAL)ty - trH / 2, (REAL)(tx1 - tx0), trH), trH / 2);
            double kx = cpsToX(tx0, tx1);
            if (kx > tx0 + 1)
                fillRound(g, C(54, 140, 255), RectF((REAL)tx0, (REAL)ty - trH / 2, (REAL)(kx - tx0), trH), trH / 2);

            // knob
            REAL kr = g_dragSlider ? 10.0f : 8.0f;
            SolidBrush kb(C(234, 240, 252));
            g.FillEllipse(&kb, (REAL)kx - kr, (REAL)ty - kr, kr * 2, kr * 2);
            Pen kp(C(54, 140, 255), 2.0f);
            g.DrawEllipse(&kp, (REAL)kx - kr, (REAL)ty - kr, kr * 2, kr * 2);
        }

        // ---- "Toggle Hotkey" row ----
        {
            RectF card((REAL)g_keyRc.left, (REAL)g_keyRc.top, (REAL)TOG_W, (REAL)TOG_H);
            fillRound(g, g_hoverKey ? C(30, 41, 70) : C(21, 29, 52), card, 13.0f);
            strokeRound(g, g_rebinding ? C(214, 168, 40) : C(40, 52, 86), 1.2f, card, 13.0f);

            // accent strip
            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, C(96, 165, 255), strip, 2.0f);
            }

            SolidBrush txt(C(228, 235, 250)), hint(C(118, 132, 168));
            g.DrawString(L"Toggle Hotkey", -1, fLabel,
                         RectF(card.X + 24, card.Y + 9, TOG_W - 120, 20), &sfL, &txt);
            g.DrawString(g_rebinding ? L"press any key  \xb7  Esc cancels" : L"click, then press a key", -1, fHint,
                         RectF(card.X + 24, card.Y + 30, TOG_W - 110, 16), &sfL, &hint);

            // keycap on the right -> the bound key (amber while listening)
            {
                REAL kw = 74, kh = 32;
                RectF cap(card.X + card.Width - kw - 14, card.Y + (TOG_H - kh) / 2, kw, kh);
                Color a = g_rebinding ? C(245, 205, 80) : C(38, 50, 86);
                Color b = g_rebinding ? C(212, 166, 38) : C(26, 35, 62);
                LinearGradientBrush kg(PointF(cap.X, cap.Y), PointF(cap.X, cap.Y + kh), a, b);
                GraphicsPath kp; float d = 8;
                kp.AddArc(cap.X, cap.Y, d, d, 180, 90);
                kp.AddArc(cap.X + cap.Width - d, cap.Y, d, d, 270, 90);
                kp.AddArc(cap.X + cap.Width - d, cap.Y + cap.Height - d, d, d, 0, 90);
                kp.AddArc(cap.X, cap.Y + cap.Height - d, d, d, 90, 90);
                kp.CloseFigure();
                g.FillPath(&kg, &kp);
                strokeRound(g, g_rebinding ? C(255, 230, 150) : C(70, 90, 140), 1.0f, cap, 8.0f);
                SolidBrush capTxt(g_rebinding ? C(40, 32, 8) : C(210, 222, 246));
                g.DrawString(g_rebinding ? L"\x2026" : g_hotkeyName, -1, fPill, cap, &sfC, &capTxt);
            }
        }

        // ---- status card ----
        {
            RectF st(22, (REAL)(g_keyRc.bottom + 12), (REAL)(WIN_W - 44), 44);
            fillRound(g, C(16, 22, 42), st, 12.0f);
            strokeRound(g, C(36, 48, 80), 1.0f, st, 12.0f);
            // state dot
            bool live = g_enabled;
            Color dot = live ? C(54, 211, 130) : C(150, 160, 190);
            SolidBrush db(dot);
            g.FillEllipse(&db, st.X + 16, st.Y + st.Height / 2 - 5, 10.0f, 10.0f);
            // status text
            wideOf(g_status, wbuf, 128);
            SolidBrush stx(C(206, 216, 238));
            g.DrawString(wbuf, -1, fStatus, RectF(st.X + 34, st.Y, st.Width - 44, st.Height), &sfL, &stx);
        }
        // ---- diagnostics line ----
        if (g_diag[0]) {
            wideOf(g_diag, wbuf, 128);
            SolidBrush tiny(C(92, 104, 138));
            g.DrawString(wbuf, -1, fTiny, RectF(0, (REAL)(WIN_H - 24), (REAL)WIN_W, 18), &sfC, &tiny);
        }
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldb);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void toggleAt(int x, int y) {
    if (inRect(g_closeRc, x, y)) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
    for (int i = 0; i < N_TOGGLES; i++) {
        if (inRect(g_togRc[i], x, y)) {
            volatile bool* s = g_toggles[i].state;
            *s = !*s;
            if (s == &g_enabled && !g_enabled) { g_physDown = false; g_physRightDown = false; }
            if (s == &g_rightClicker && !g_rightClicker) g_physRightDown = false;
            logf("[ac] toggle %ls -> %s\n", g_toggles[i].label, *s ? "ON" : "OFF");
            requestRepaint();
            return;
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 140, nullptr);   // keep the status line fresh
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;                          // we fully paint -> no flicker
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_dragSlider) {                     // live-drag the click-speed knob
            double tx0, tx1, ty; sliderTrack(&tx0, &tx1, &ty);
            setCpsFromX((double)x, tx0, tx1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        int hov = -1;
        for (int i = 0; i < N_TOGGLES; i++) if (inRect(g_togRc[i], x, y)) { hov = i; break; }
        bool hc = inRect(g_closeRc, x, y);
        bool hk = inRect(g_keyRc, x, y);
        if (hov != g_hover || hc != g_hoverClose || hk != g_hoverKey) {
            g_hover = hov; g_hoverClose = hc; g_hoverKey = hk;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1 || g_hoverClose || g_hoverKey) {
            g_hover = -1; g_hoverClose = false; g_hoverKey = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (inRect(g_sliderRc, x, y)) {         // grab the click-speed slider
            g_dragSlider = true;
            SetCapture(hwnd);
            double tx0, tx1, ty; sliderTrack(&tx0, &tx1, &ty);
            setCpsFromX((double)x, tx0, tx1);
            requestRepaint();
            return 0;
        }
        if (inRect(g_keyRc, x, y)) {            // start / cancel hotkey capture
            g_rebinding = !g_rebinding;
            requestRepaint();
            return 0;
        }
        bool onToggle = inRect(g_closeRc, x, y);
        for (int i = 0; i < N_TOGGLES; i++) if (inRect(g_togRc[i], x, y)) onToggle = true;
        if (onToggle) { toggleAt(x, y); return 0; }
        if (y < 104) {  // title strip -> drag the window
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragSlider) { g_dragSlider = false; ReleaseCapture(); requestRepaint(); }
        return 0;
    case WM_CLOSE:
        g_running = false;                 // tells the worker to unload everything
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI guiThread(LPVOID) {
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = (HINSTANCE)g_self;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BotClickerWnd";
    RegisterClassExW(&wc);

    layoutToggles();
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, L"BotClickerWnd", L"BotClicker",
                                WS_POPUP, sx - WIN_W - 40, 60, WIN_W, WIN_H,
                                nullptr, nullptr, (HINSTANCE)g_self, nullptr);
    if (!hwnd) { GdiplusShutdown(g_gdipToken); return 1; }
    g_hwnd = hwnd;

    // rounded window shape + slight translucency for a modern feel
    HRGN rgn = CreateRoundRectRgn(0, 0, WIN_W + 1, WIN_H + 1, 22, 22);
    SetWindowRgn(hwnd, rgn, TRUE);
    SetLayeredWindowAttributes(hwnd, 0, 247, LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    g_hwnd = nullptr;
    // Unregister the class so a later re-inject doesn't reuse a WndProc that points into the
    // soon-to-be-unmapped DLL (otherwise re-injection creates a window over freed code).
    UnregisterClassW(L"BotClickerWnd", (HINSTANCE)g_self);
    GdiplusShutdown(g_gdipToken);
    return 0;
}

// ---- main agent thread -----------------------------------------------------
static void run() {
    logf("[ac] attached to pid %lu. opening UI + resolving JVM...\n", GetCurrentProcessId());

    HANDLE guiT = CreateThread(nullptr, 0, guiThread, nullptr, 0, nullptr);

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
        setStatus("not a Java process?");
    } else {
        setupJvm(env);
    }

    logf("[ac] running at %.1f CPS (slider 0-30). J or the Autoclicker button arms it. END unloads.\n", g_cps);

    timeBeginPeriod(1);   // 1 ms scheduler resolution -> tight, low-jitter ticks (paired below)
    g_hookThread = CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);
    HANDLE potT  = CreateThread(nullptr, 0, potThread,  nullptr, 0, nullptr);

    using clock = std::chrono::high_resolution_clock;
    auto nextTick  = clock::now();
    auto lastClick = clock::now() - std::chrono::hours(1);  // "long ago" -> first click fires at once
    auto lastRight = lastClick;

    int  lastState = -99;
    bool lastKey = false;

    while (g_running) {
        if (GetAsyncKeyState(VK_END) & 0x8000) { logf("[ac] END pressed -- unloading.\n"); break; }

        g_gameWindow = findGameWindow();

        // Hotkey rebinding: grab the next key pressed (Esc cancels; mouse/Esc/End can't bind).
        if (g_rebinding) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { g_rebinding = false; requestRepaint(); }
            else for (int vk = 0x08; vk <= 0xFE; vk++) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                    vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == VK_END) continue;
                if (GetAsyncKeyState(vk) & 0x8000) {
                    g_hotkey = vk; computeHotkeyName(vk);
                    g_rebinding = false; lastKey = true;   // don't let this same press also toggle
                    logf("[ac] hotkey -> %ls (0x%02X)\n", g_hotkeyName, vk);
                    requestRepaint();
                    break;
                }
            }
        }

        // Hotkey toggle: edge-detected, only when focused and not typing in a screen/chat.
        bool keyDown = (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
        if (!g_rebinding && keyDown && !lastKey && gameFocused() && !(env && screenOpen(env))) {
            g_enabled = !g_enabled;
            if (!g_enabled) { g_physDown = false; g_physRightDown = false; }  // drop held state
            logf("[ac] %s\n", g_enabled ? "ENABLED" : "DISABLED");
            requestRepaint();
            lastState = -99;
        }
        lastKey = keyDown;

        // Live click rate from the GUI slider. We tick at 1 ms and gate clicks by elapsed
        // high-resolution time, so the CPS is accurate and not bound to Sleep granularity.
        double cps = g_cps;
        bool   off = (cps < CPS_OFF_EPS);
        double baseGap = off ? 1e9 : (1000.0 / cps);    // ms between clicks at the current CPS
        auto   now     = clock::now();
        auto   msSince = [&](const clock::time_point& t) {
            return std::chrono::duration<double, std::milli>(now - t).count();
        };

        int state = 0;
        if (g_enabled && g_physDown && gameFocused() && !off) {
            if (env && screenOpen(env)) {
                // In a screen (inventory/chest/...). No blocks here. Only when Inv. Throttle is on
                // AND the hotbar is full do we space clicks past the double-click window.
                if (g_invThrottle && hotbarFull(env)) {
                    double gap = baseGap > (double)INVENTORY_MIN_MS ? baseGap : (double)INVENTORY_MIN_MS;
                    if (msSince(lastClick) >= gap) { sendClick(); lastClick = now; }
                    state = 6;
                } else {
                    if (msSince(lastClick) >= baseGap) { sendClick(); lastClick = now; }
                    state = 4;
                }
            } else {
                int eval = env ? evalTarget(env) : EVAL_ALLOW;
                if (eval == EVAL_BLOCK    && !g_blockGuard) eval = EVAL_ALLOW;  // guard off -> mine away
                if (eval == EVAL_BACKTURN && !g_backTurn)   eval = EVAL_ALLOW;  // slow off -> full CPS
                if (eval == EVAL_BLOCK) {
                    state = 2;  // aimed at a block -> deliberately doing nothing
                } else {
                    double gap = baseGap;
                    if (eval == EVAL_BACKTURN) {
                        double bt = 1000.0 / BACKTURN_CPS;   // back turned -> the slower of the two rates
                        if (bt > gap) gap = bt;
                    }
                    if (msSince(lastClick) >= gap) { sendClick(); lastClick = now; }
                    state = (eval == EVAL_BACKTURN) ? 5 : (g_canDetectBlocks ? 1 : 3);
                }
            }
        } else if (!gameFocused()) {
            g_physDown = false;     // avoid a stuck "down" if focus was lost mid-hold
        }

        // Right clicker: independent of the left logic, gated at the same CPS.
        bool rightActive = false;
        if (g_enabled && g_rightClicker && g_physRightDown && gameFocused() && !off) {
            if (msSince(lastRight) >= baseGap) { sendRightClick(); lastRight = now; }
            rightActive = true;
        } else if (!gameFocused()) {
            g_physRightDown = false;
        }

        // Status line. Combine left `state` with the right-click flag so toggling either reprints.
        int combo = state * 2 + (rightActive ? 1 : 0);
        if (combo != lastState) {
            bool leftClicking = (state == 1 || state == 3 || state == 4 || state == 5 || state == 6);
            if      (!g_enabled)             setStatus("disarmed \xc2\xb7 hotkey or button to arm");
            else if (rightActive && leftClicking) setStatus("left + right clicking");
            else if (rightActive)            setStatus("right-clicking \xc2\xb7 hold RMB");
            else if (state == 0) setStatus("armed \xc2\xb7 idle (hold LMB)");
            else if (state == 1) setStatus("clicking \xc2\xb7 entity / air");
            else if (state == 2) setStatus("aimed at block \xc2\xb7 holding fire");
            else if (state == 3) setStatus("clicking \xc2\xb7 no block-detect");
            else if (state == 4) setStatus("clicking \xc2\xb7 inventory");
            else if (state == 5) setStatus("back turned \xc2\xb7 6 CPS");
            else if (state == 6) setStatus("inventory \xc2\xb7 hotbar full");
            lastState = combo;
        }

        nextTick += std::chrono::milliseconds(1);
        auto tnow = clock::now();
        if (nextTick < tnow) nextTick = tnow;   // long JNI work this tick -> resync, don't burst-catch-up
        std::this_thread::sleep_until(nextTick);
    }

    timeEndPeriod(1);
    g_running = false;     // signal to every DLL-resident thread to wind down

    // Every thread that runs code inside this DLL must be fully stopped BEFORE FreeLibrary
    // unmaps it -- otherwise a still-live thread executes freed code and crashes the game.

    // 1) Input hook FIRST: it's the one piece of DLL code the OS can invoke asynchronously
    //    (on the next mouse event), so it's the most dangerous thing to leave installed.
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) { WaitForSingleObject(g_hookThread, 4000); CloseHandle(g_hookThread); }

    // 2) GUI window/thread (on the END / hotkey path it isn't closing yet, so ask it to).
    //    The GUI thread unregisters its window class on the way out.
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (guiT) { WaitForSingleObject(guiT, 4000); CloseHandle(guiT); }

    // 3) Pot thread (exits on g_running == false; frees its own screen ref + detaches).
    if (potT) { WaitForSingleObject(potT, 4000); CloseHandle(potT); }

    // 4) Release JVM global refs (so repeated inject -> close -> re-inject doesn't leak) and
    //    detach this thread. Safe now that no other DLL thread is touching the JVM.
    if (env) {
        if (g_mcInstance)        env->DeleteGlobalRef(g_mcInstance);
        if (g_mcClass)           env->DeleteGlobalRef(g_mcClass);
        if (g_mopClass)          env->DeleteGlobalRef(g_mopClass);
        if (g_entityClass)       env->DeleteGlobalRef(g_entityClass);
        if (g_guiContainerClass) env->DeleteGlobalRef(g_guiContainerClass);
        if (g_itemPotionClass)   env->DeleteGlobalRef(g_itemPotionClass);
    }
    if (env && g_jvm) g_jvm->DetachCurrentThread();

    // 5) Drop our last module ref and exit -> DLL unmaps cleanly, re-inject works.
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
