// payload.cpp  --  JVMTI/JNI agent: a left-click autoclicker that never mines blocks.
//
// Injected via injector.exe into javaw.exe (Minecraft 1.8.9 Forge). On attach it grabs
// the running JVM (JNI_GetCreatedJavaVMs), gets a JVMTI env, and locates a few Minecraft
// fields so it can read what the player is looking at.
//
// UI: instead of a text console, the agent opens "BotClicker" -- a small frameless,
// always-on-top GDI+ window (dark space-blue theme). Each feature is a toggle button that
// glows GREEN when on and RED when off. Drag it by the title bar; the little X
// unloads cleanly.
//
// Toggles:
//   * Autoclicker   -- master arm/disarm (also bound to the J key). Off by default.
//   * Back-Turn Slow-- drop to a low CPS when the targeted player's back is turned.
// Splash Pot Move is always on: in a container GUI, while you hold Shift, click splash potions
// you hover into the hotbar. Runs on its own fast thread, not the CPS pacing.
//
// Behaviour (only while Minecraft is the foreground window):
//   * A low-level mouse hook watches your real left-click: in a container GUI it seizes
//     Shift+LMB to inject clean clicks; in the world it passes the hold through and layers
//     our own click-edges on top.
//   * While armed and holding LMB we inject clicks at the slider-set rate (drag 0-30 CPS,
//     default 15.8). Back-Turn paces those clicks when a targeted player's back is turned.
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
static jfieldID      g_fLeftClickCounter = nullptr; // Minecraft.leftClickCounter, vanilla miss-hit delay
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

// For "is the hotbar full" -> EntityPlayer.inventory.mainInventory[0..8]. Used by Splash Pot Move
// to decide whether a hovered pot can still be shift-moved to the hotbar.
static jfieldID      g_fInventory = nullptr;  // EntityPlayer.inventory
static jfieldID      g_fMainInv   = nullptr;  // InventoryPlayer.mainInventory (ItemStack[])
static bool          g_canDetectHotbar = false;

// For "splash-pot shift-click": while the user is physically holding Shift and hovering a splash
// potion in a container GUI, click it toward the hotbar. Resolved lazily (the GUI/Slot classes may
// not be loaded until the first container is opened). Runs on its own fast thread.
static jclass        g_guiContainerClass = nullptr; // global ref: GuiContainer
static jfieldID      g_fTheSlot   = nullptr;        // GuiContainer.theSlot (hovered Slot)
static jfieldID      g_fSlotNumber= nullptr;        // Slot.slotNumber
static jfieldID      g_fSlotInventory = nullptr;    // Slot.inventory
static jmethodID     g_midGetStack= nullptr;        // Slot.getStack()
static jmethodID     g_midGetItem = nullptr;        // ItemStack.getItem()
static jmethodID     g_midGetMeta = nullptr;        // ItemStack.getMetadata()
static jclass        g_itemPotionClass = nullptr;   // global ref: ItemPotion
static bool          g_canPotMove = false;          // all of the above resolved
static jobject       g_lastInvScreen = nullptr;     // global ref: detect inventory reopen (pot thread only)

// Right-clicker gate: only auto-right-click while a block or bucket is the selected hotbar item, so
// a plain right-click still works for swords, food, bows, ender pearls, etc. The (JNI-less) mouse
// hook can't call into the JVM, so the worker refreshes g_holdingPlaceable each tick and the hook
// just reads that cached flag.
static jfieldID      g_fCurrentItem    = nullptr;   // InventoryPlayer.currentItem (selected hotbar slot 0-8)
static jmethodID     g_midGetCurItem   = nullptr;   // InventoryPlayer.getCurrentItem() (independent fallback)
static jclass        g_itemBlockClass  = nullptr;   // global ref: ItemBlock (covers its subclasses)
static jclass        g_itemBucketClass = nullptr;   // global ref: ItemBucket (empty/water/lava)
static bool          g_canDetectHeld   = false;     // enough held-item refs resolved to read the held item
static ULONGLONG     g_potLastClick[256] = {};      // last shift-click attempt per slot
static volatile bool g_potHoldClick = false;        // pot thread -> main loop: hovering a pot, don't normal-click it

static volatile HWND g_gameWindow = nullptr;
static volatile bool g_physDown      = false; // real (non-injected) LMB state
static volatile bool g_physRightDown = false; // real (non-injected) RMB state
// A container/menu screen is open (worker updates this every tick). It decides how the mouse hook
// treats the LEFT button: in a SCREEN we seize it (inventory autoclicker injects clean clicks); in
// the WORLD we let the real held button pass straight through to the game. That pass-through is the
// key to seamless mining -- a held click digs naturally, and the combat autoclicker just layers
// extra click-edges on top of that continuous hold, so combat<->mining needs no release/re-press.
static volatile bool g_screenOpen = false;
static volatile bool g_guiLeftSeized = false;   // true after swallowing a Shift+LMB GUI press
static volatile bool g_guiRightSeized = false;  // true after swallowing a Shift+RMB GUI press
static volatile bool g_holdingPlaceable = false; // worker->hook: selected hotbar item is a block/bucket
static HHOOK         g_mouseHook   = nullptr;
static HANDLE        g_hookThread  = nullptr;
static DWORD         g_hookThreadId = 0;

// ---- toggleable options (shared with the GUI) ------------------------------
static volatile bool g_enabled     = false;   // master arm; starts off
static volatile bool g_rightClicker= false;   // also rapid right-click while holding RMB
static volatile bool g_backTurn    = false;   // optional slow CPS on a back-turned target
static volatile bool g_potMove     = true;    // always shift-click splash pots you hover in a GUI

// Master-toggle hotkey. Rebindable from the GUI: click the row, then press any key.
static volatile int  g_hotkey      = 'J';
static wchar_t       g_hotkeyName[32] = L"J";
static volatile bool g_rebinding   = false;   // currently capturing a new key

// Click rate, adjustable live from the GUI slider (drag 0..30 CPS). 0 = effectively paused.
static volatile double g_cps     = 15.8;
static const double  CPS_MIN     = 0.0;
static const double  CPS_MAX     = 30.0;
static const double  CPS_OFF_EPS = 0.1;          // below this we treat the clicker as paused
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
    if (vk == 0) { wcscpy_s(g_hotkeyName, L"None"); return; }   // no key bound
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
    // Treat our own overlay as "the game has focus" so the hotkey toggle still fires while a
    // toggle/slider is being clicked. NOTE: do NOT use this to gate click injection -- see
    // gameForeground() below, or we'd inject clicks onto our own GUI.
    return g && (fg == g || fg == g_hwnd);
}
// Strictly the real game window in the foreground -- NOT our overlay. Click injection is gated on
// this so the autoclicker only fires while Minecraft itself is the active window; interacting with
// the BotClicker GUI never triggers (or receives) injected clicks.
static inline bool gameForeground() {
    HWND g = g_gameWindow;
    return g && GetForegroundWindow() == g;
}

static inline bool physicalShiftHeld() {
    return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
}

// ---- low-level mouse hook: own the left button while MC is focused ----------
static LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        bool injected = (m->flags & LLMHF_INJECTED) != 0;   // our own SendInput clicks
        // Only seize buttons while armed AND the game (not our overlay) is focused.
        if (!injected && g_enabled && g_gameWindow && GetForegroundWindow() == g_gameWindow) {
            bool screen = g_screenOpen;
            bool shift = physicalShiftHeld();

            // Left button: in GUIs, only Shift+LMB enters autoclicker capture. Plain GUI clicks pass
            // through normally. In the world, the real button passes through and the worker layers
            // combat click-edges on top of that continuous hold.
            if (w == WM_LBUTTONDOWN) {
                if (screen) {
                    g_guiLeftSeized = shift;
                    g_physDown = shift;
                    if (shift) return 1;
                } else {
                    g_guiLeftSeized = false;
                    g_physDown = true;
                }
            } else if (w == WM_LBUTTONUP) {
                bool swallow = g_guiLeftSeized;
                g_physDown = false;
                g_guiLeftSeized = false;
                if (swallow) return 1;
            }
            else if (g_rightClicker) {
                // Only take over the right button when auto-clicking actually makes sense: a block or
                // bucket in hand out in the world, or Shift held in a container GUI. Anything else
                // (sword, food, bow, pearls, plain GUI clicks) passes straight through, so its normal
                // right-click still works. g_holdingPlaceable is refreshed by the worker each tick.
                if (w == WM_RBUTTONDOWN) {
                    bool seize = screen ? shift : g_holdingPlaceable;
                    if (seize) {
                        g_guiRightSeized = screen;
                        g_physRightDown = true;
                        return 1;
                    }
                    g_guiRightSeized = false;
                    g_physRightDown = false;
                }
                if (w == WM_RBUTTONUP) {
                    bool swallow = g_physRightDown;   // only swallow the up of a hold we actually seized
                    g_physRightDown = false;
                    g_guiRightSeized = false;
                    if (swallow) return 1;
                }
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

// Single left transition (not a full click). Used by the in-world state machine to drive the
// button's sustained state on top of the user's real hold: hold it DOWN to mine, UP to suppress.
static void leftEdge(bool down) {
    INPUT in = {}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
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

static bool ensureHeldRefs(JNIEnv* env);   // defined below; setupJvm resolves it eagerly for diagnostics

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
    g_fLeftClickCounter = findField(g_mcClass, "leftClickCounter", "field_71429_W", nullptr);

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
    logf("[ac] vanilla hit-delay reset: %s\n", g_fLeftClickCounter ? "ok" : "MISS");

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
    logf("[ac] hotbar detection: %s\n", g_canDetectHotbar ? "ok" : "MISS -> pots move regardless of hotbar");

    // Held-item detection for the right-clicker (block/bucket gate). Resolve it now so its status
    // shows in the diagnostics line; it also retries lazily if a class wasn't loaded this early.
    ensureHeldRefs(env);
    logf("[ac] held-item detection: %s\n", g_canDetectHeld ? "ok" : "MISS -> right-clicker stays idle");

    _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE,
                "blocks %s \xc2\xb7 delay %s \xc2\xb7 facing %s \xc2\xb7 hotbar %s \xc2\xb7 held %s",
                g_canDetectBlocks ? "ok" : "off", g_fLeftClickCounter ? "ok" : "off",
                g_canDetectFacing ? "ok" : "off", g_canDetectHotbar ? "ok" : "off",
                g_canDetectHeld ? "ok" : "off");
    requestRepaint();
    return true;
}

static void clearLeftClickDelay(JNIEnv* env) {
    if (!env || !g_mcInstance || !g_fLeftClickCounter) return;
    env->SetIntField(g_mcInstance, g_fLeftClickCounter, 0);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Are all 9 hotbar slots (mainInventory[0..8]) occupied? In 1.8.9 an empty slot is null.
// If we can't read the inventory, report "not full" so pots still move.
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

// Resolve everything needed to read the player's selected hotbar item. TWO independent ways to fetch
// the held stack are resolved -- mainInventory[currentItem] AND InventoryPlayer.getCurrentItem() --
// so a single unmapped name can't kill the feature. Plus ItemStack.getItem() (shared with the pot
// path) and the ItemBlock / ItemBucket classes. MCP names first, then SRG: works dev and prod.
static bool ensureHeldRefs(JNIEnv* env) {
    if (g_canDetectHeld) return true;
    jclass invC = findLoadedClass(env, "Lnet/minecraft/entity/player/InventoryPlayer;");
    if (invC) {
        if (!g_fCurrentItem)  g_fCurrentItem  = findField(invC, "currentItem",   "field_70461_c", nullptr);
        if (!g_fMainInv)      g_fMainInv      = findField(invC, "mainInventory", "field_70462_a", nullptr);
        if (!g_midGetCurItem) g_midGetCurItem = findMethod(env, invC, "getCurrentItem", "func_70448_g",
                                                           "()Lnet/minecraft/item/ItemStack;");
        env->DeleteGlobalRef(invC);
    }
    if (!g_midGetItem) {
        jclass stackC = findLoadedClass(env, "Lnet/minecraft/item/ItemStack;");
        if (stackC) {
            g_midGetItem = findMethod(env, stackC, "getItem", "func_77973_b",
                                      "()Lnet/minecraft/item/Item;");
            env->DeleteGlobalRef(stackC);
        }
    }
    if (!g_itemBlockClass)  g_itemBlockClass  = findLoadedClass(env, "Lnet/minecraft/item/ItemBlock;");
    if (!g_itemBucketClass) g_itemBucketClass = findLoadedClass(env, "Lnet/minecraft/item/ItemBucket;");

    bool canFetch = (g_fMainInv && g_fCurrentItem) || g_midGetCurItem;   // either path is enough
    g_canDetectHeld = g_fThePlayer && g_fInventory && g_midGetItem &&
                      g_itemBlockClass && g_itemBucketClass && canFetch;
    return g_canDetectHeld;
}

// The player's selected hotbar ItemStack (a local ref the caller must release), or null for an empty
// hand. Prefers mainInventory[currentItem]; falls back to getCurrentItem() if that path isn't mapped.
static jobject heldStack(JNIEnv* env, jobject inv) {
    if (g_fMainInv && g_fCurrentItem) {
        jint sel = env->GetIntField(inv, g_fCurrentItem);
        jobjectArray main = static_cast<jobjectArray>(env->GetObjectField(inv, g_fMainInv));
        if (main) {
            jobject st = (sel >= 0 && sel < 9) ? env->GetObjectArrayElement(main, sel) : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); st = nullptr; }
            env->DeleteLocalRef(main);
            if (st) return st;
        }
    }
    if (g_midGetCurItem) {
        jobject st = env->CallObjectMethod(inv, g_midGetCurItem);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return st;
    }
    return nullptr;
}

// True only if the player's selected hotbar item is a block or a (water/lava/empty) bucket. If we
// can't read the item we return FALSE: auto-right-clicking an unknown item (bow, pearls, food) is
// worse than not firing.
static bool holdingPlaceable(JNIEnv* env) {
    if (!ensureHeldRefs(env)) return false;
    jobject me = env->GetObjectField(g_mcInstance, g_fThePlayer);
    if (!me) return false;
    jobject inv = env->GetObjectField(me, g_fInventory);
    env->DeleteLocalRef(me);
    if (!inv) return false;
    jobject stack = heldStack(env, inv);
    env->DeleteLocalRef(inv);
    if (!stack) return false;                        // empty hand
    jobject item = env->CallObjectMethod(stack, g_midGetItem);
    env->DeleteLocalRef(stack);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!item) return false;
    bool placeable = env->IsInstanceOf(item, g_itemBlockClass) ||
                     env->IsInstanceOf(item, g_itemBucketClass);
    env->DeleteLocalRef(item);
    return placeable;
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
static int evalTarget(JNIEnv* env, bool detectBackTurn) {
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
            else if (ord == 2 && detectBackTurn && targetBackTurned(env, mop)) result = EVAL_BACKTURN;
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
        if (!g_fSlotInventory) g_fSlotInventory = findField(slotC, "inventory", "field_75224_c", nullptr);
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

    g_canPotMove = g_fTheSlot && g_fSlotNumber && g_fSlotInventory && g_midGetStack &&
                   g_midGetItem && g_midGetMeta && g_itemPotionClass;
    if (g_canPotMove) logf("[ac] pot shift-click: handles resolved\n");
    return g_canPotMove;
}

static bool playerHotbarSlot(JNIEnv* env, jobject slot, jint slotNumber) {
    if (slotNumber < 0 || slotNumber > 8 || !g_fSlotInventory ||
        !g_mcInstance || !g_fThePlayer || !g_fInventory) {
        return false;
    }

    jobject slotInv = env->GetObjectField(slot, g_fSlotInventory);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (slotInv) env->DeleteLocalRef(slotInv); return false; }
    jobject me = env->GetObjectField(g_mcInstance, g_fThePlayer);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (slotInv) env->DeleteLocalRef(slotInv); return false; }
    jobject playerInv = me ? env->GetObjectField(me, g_fInventory) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (playerInv) env->DeleteLocalRef(playerInv); playerInv = nullptr; }

    bool isHotbar = slotInv && playerInv && env->IsSameObject(slotInv, playerInv);
    if (playerInv) env->DeleteLocalRef(playerInv);
    if (me) env->DeleteLocalRef(me);
    if (slotInv) env->DeleteLocalRef(slotInv);
    return isHotbar;
}

// Is the slot currently under the cursor a splash potion? Called from the MAIN loop right before
// it would send an inventory click, so the "is this a pot?" test and the click happen on the same
// thread back-to-back -- no cross-thread lag where a plain click could grab a pot. Returns false
// if the pot handles aren't resolved yet (then it just clicks normally).
static bool hoveredSplashPot(JNIEnv* env, jint* outSlotNumber = nullptr, bool* outPlayerHotbar = nullptr) {
    if (outSlotNumber) *outSlotNumber = -1;
    if (outPlayerHotbar) *outPlayerHotbar = false;
    if (!g_canPotMove || !g_mcInstance || !g_fCurrentScreen) return false;
    jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
    if (!scr) return false;
    bool isPot = false;
    if (env->IsInstanceOf(scr, g_guiContainerClass)) {
        jobject slot = env->GetObjectField(scr, g_fTheSlot);
        if (slot) {
            jint slotNumber = env->GetIntField(slot, g_fSlotNumber);
            if (outSlotNumber) *outSlotNumber = slotNumber;
            if (outPlayerHotbar) *outPlayerHotbar = playerHotbarSlot(env, slot, slotNumber);
            jobject stack = env->CallObjectMethod(slot, g_midGetStack);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (stack) {
                jobject item = env->CallObjectMethod(stack, g_midGetItem);
                if (env->ExceptionCheck()) env->ExceptionClear();
                jint meta = env->CallIntMethod(stack, g_midGetMeta);
                if (env->ExceptionCheck()) env->ExceptionClear();
                isPot = item && env->IsInstanceOf(item, g_itemPotionClass) && (meta & 0x4000) != 0;
                if (item) env->DeleteLocalRef(item);
                env->DeleteLocalRef(stack);
            }
            env->DeleteLocalRef(slot);
        }
    }
    env->DeleteLocalRef(scr);
    return isPot;
}

static inline bool potClickReady(jint slotNumber, ULONGLONG nowMs) {
    if (slotNumber < 0 || slotNumber >= 256) return true;
    ULONGLONG last = g_potLastClick[slotNumber];
    return last == 0 || nowMs - last >= 325;
}

static bool hoveredPotOwnedByPotMove(JNIEnv* env) {
    bool playerHotbar = false;
    if (!hoveredSplashPot(env, nullptr, &playerHotbar)) return false;
    if (playerHotbar) return true;  // protect hotbar pots from being shift-clicked back out
    return !hotbarFull(env);
}

static void clearPotSession(JNIEnv* env) {
    if (g_lastInvScreen) {
        env->DeleteGlobalRef(g_lastInvScreen);
        g_lastInvScreen = nullptr;
    }
    memset(g_potLastClick, 0, sizeof(g_potLastClick));
}

// Dedicated fast thread (~tick-rate, NOT gated by the CPS pacing): only while the autoclicker is
// actively left-clicking (armed + holding LMB), the user is physically holding Shift, and a
// container GUI is open. When the hovered slot holds a splash potion and the hotbar has room, it
// (a) tells the main loop to hold fire on that slot via g_potHoldClick so the autoclicker can't
// double-click/gather the pot, and (b) clicks it to the hotbar using the user's held Shift.
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
        // Only move pots while the user is actually left-clicking and physically holding Shift.
        bool baseActive = g_potMove && g_enabled && g_physDown && g_mcInstance && g_fCurrentScreen &&
                          g_gameWindow && GetForegroundWindow() == g_gameWindow;
        bool active = baseActive && physicalShiftHeld();
        bool block = false;   // hovering a splash pot this tick -> main loop must not normal-click it

        if (!g_mcInstance || !g_fCurrentScreen) {
            clearPotSession(env);
        } else if (baseActive || g_lastInvScreen) {
            jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
            if (scr) {
                bool isContainer = ensurePotRefs(env) && env->IsInstanceOf(scr, g_guiContainerClass);
                if (isContainer) {
                    // Inventory (re)opened? -> clear retry state for the new container.
                    if (!g_lastInvScreen || !env->IsSameObject(scr, g_lastInvScreen)) {
                        if (g_lastInvScreen) env->DeleteGlobalRef(g_lastInvScreen);
                        g_lastInvScreen = env->NewGlobalRef(scr);
                        memset(g_potLastClick, 0, sizeof(g_potLastClick));
                    }
                    jobject slot = env->GetObjectField(scr, g_fTheSlot);  // hovered slot, or null
                    if (slot) {
                        jint sn = env->GetIntField(slot, g_fSlotNumber);
                        // Read the slot every tick (even if already clicked) so we keep holding while
                        // the pot is still there -- it only stops once the click has actually moved it.
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
                            if (active && splash) {
                                // Hotbar pots are already where they belong; hold fire so Shift+LMB
                                // sweeps don't move them back out. If the hotbar is full and this
                                // is not a hotbar slot, stand down so normal GUI clicking continues.
                                bool inPlayerHotbar = playerHotbarSlot(env, slot, sn);
                                if (inPlayerHotbar) {
                                    block = true;
                                } else if (!hotbarFull(env)) {
                                    block = true;
                                    ULONGLONG nowMs = GetTickCount64();
                                    if (potClickReady(sn, nowMs)) {
                                        sendClick();   // instant left click, user's Shift held -> quick-move
                                        if (sn >= 0 && sn < 256) g_potLastClick[sn] = nowMs;
                                        logf("[ac] pot shift-click slot %d (meta 0x%X)\n", (int)sn, (unsigned)meta);
                                    }
                                }
                            }
                            if (item) env->DeleteLocalRef(item);
                            env->DeleteLocalRef(stack);
                        }
                        env->DeleteLocalRef(slot);
                    }
                } else if (g_lastInvScreen) {
                    clearPotSession(env);
                }
                env->DeleteLocalRef(scr);
            } else if (g_lastInvScreen) {
                // No screen open -> inventory closed; drop retry state + tracked screen.
                clearPotSession(env);
            }
        }

        g_potHoldClick = block;

        nextTick += std::chrono::milliseconds(1);   // independent of the click-speed slider
        auto tnow = clock::now();
        if (nextTick < tnow) nextTick = tnow;
        std::this_thread::sleep_until(nextTick);
    }

    clearPotSession(env);
    if (g_jvm) g_jvm->DetachCurrentThread();
    return 0;
}

// ============================================================================
//  BotClicker GUI  --  frameless GDI+ window, dark space-blue, red/green toggles
// ============================================================================
using namespace Gdiplus;

static const int   WIN_W = 360, WIN_H = 586;   // 3 toggles + slider + hotkey + status
static const int   TOG_X = 22, TOG_W = WIN_W - 44, TOG_H = 54, TOG_GAP = 12, TOG_Y0 = 120;
static const int   N_TOGGLES = 3;

struct ToggleDef { const wchar_t* label; const wchar_t* hint; volatile bool* state; };
static ToggleDef g_toggles[N_TOGGLES] = {
    { L"Autoclicker",    L"left-click (hotkey)", &g_enabled     },
    { L"Right Clicker",  L"blocks & buckets only", &g_rightClicker},
    { L"Back-Turn Slow", L"ease off back-stabs", &g_backTurn    },
};
static RECT g_togRc[N_TOGGLES];
static RECT g_sliderRc;              // the "Click Speed" slider row
static bool g_dragSlider = false;    // currently dragging the slider knob
static bool g_cpsEdit    = false;    // right-clicked the slider -> typing a CPS value
static wchar_t g_cpsBuf[16] = L"";   // the digits being typed while g_cpsEdit
static RECT g_keyRc;                 // the "Toggle Hotkey" row
static RECT g_closeRc = { WIN_W - 40, 14, WIN_W - 16, 38 };
static RECT g_minRc   = { WIN_W - 72, 14, WIN_W - 48, 38 };   // minimize, left of close
static const int COLLAPSED_H = 64;   // window height while minimized (just the title bar)
static bool g_minimized = false;     // collapsed to the title bar
static int  g_hover = -1;            // hovered toggle index, or -1
static bool g_hoverClose = false;
static bool g_hoverMin   = false;
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
// Parse the typed buffer into g_cps (clamped 0..30, snapped to 0.1) and leave edit mode.
// Empty/garbage input just cancels without changing the value.
static void commitCpsEdit() {
    if (g_cpsBuf[0]) {
        double v = _wtof(g_cpsBuf);
        if (v < CPS_MIN) v = CPS_MIN;
        if (v > CPS_MAX) v = CPS_MAX;
        g_cps = floor(v * 10.0 + 0.5) / 10.0;
        logf("[ac] cps typed -> %.1f\n", g_cps);
    }
    g_cpsEdit = false; g_cpsBuf[0] = 0;
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

        // ---- minimize / restore button (left of close) ----
        {
            RectF mr((REAL)g_minRc.left, (REAL)g_minRc.top,
                     (REAL)(g_minRc.right - g_minRc.left), (REAL)(g_minRc.bottom - g_minRc.top));
            fillRound(g, g_hoverMin ? C(54, 86, 150) : C(34, 44, 72), mr, 7.0f);
            Pen mp(g_hoverMin ? C(235, 242, 255) : C(150, 165, 200), 1.8f);
            REAL pad = 7;
            if (g_minimized) {  // restore glyph: a small square
                g.DrawRectangle(&mp, mr.X + pad, mr.Y + pad, mr.Width - 2 * pad, mr.Height - 2 * pad);
            } else {            // minimize glyph: a dash near the bottom
                g.DrawLine(&mp, mr.X + pad, mr.Y + mr.Height - pad, mr.X + mr.Width - pad, mr.Y + mr.Height - pad);
            }
        }

        wchar_t wbuf[128];
        if (g_minimized) {
            // collapsed: only the title bar + buttons are shown; skip the body entirely.
        } else {
        // ---- toggle buttons ----
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
            fillRound(g, (g_dragSlider || g_cpsEdit) ? C(30, 41, 70) : C(21, 29, 52), card, 13.0f);
            strokeRound(g, g_cpsEdit ? C(214, 168, 40) : (g_dragSlider ? C(96, 165, 255) : C(40, 52, 86)), 1.2f, card, 13.0f);

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
                wchar_t vb[40];
                if (g_cpsEdit)               swprintf_s(vb, L"%s_ CPS", g_cpsBuf);   // typed value + caret
                else if (g_cps < CPS_OFF_EPS) swprintf_s(vb, L"off");
                else                         swprintf_s(vb, L"%.1f CPS", g_cps);
                StringFormat sfR; sfR.SetAlignment(StringAlignmentFar); sfR.SetLineAlignment(StringAlignmentCenter);
                SolidBrush vcol(g_cpsEdit ? C(245, 205, 80) : C(96, 165, 255));
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
            g.DrawString(g_rebinding ? L"press any key  \xb7  Esc clears bind" : L"click, then press a key", -1, fHint,
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

// Collapse to / expand from the title bar. Resizes the window and reshapes its rounded region.
static void applyMinimizedState(HWND hwnd) {
    int h = g_minimized ? COLLAPSED_H : WIN_H;
    SetWindowPos(hwnd, nullptr, 0, 0, WIN_W, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN rgn = CreateRoundRectRgn(0, 0, WIN_W + 1, h + 1, 22, 22);
    SetWindowRgn(hwnd, rgn, TRUE);   // window takes ownership of rgn
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void toggleAt(int x, int y) {
    if (inRect(g_closeRc, x, y)) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
    if (inRect(g_minRc, x, y)) { g_minimized = !g_minimized; applyMinimizedState(g_hwnd); return; }
    for (int i = 0; i < N_TOGGLES; i++) {
        if (inRect(g_togRc[i], x, y)) {
            volatile bool* s = g_toggles[i].state;
            *s = !*s;
            if (s == &g_enabled && !g_enabled) {
                g_physDown = false; g_physRightDown = false;
                g_guiLeftSeized = false; g_guiRightSeized = false;
            }
            if (s == &g_rightClicker && !g_rightClicker) {
                g_physRightDown = false;
                g_guiRightSeized = false;
            }
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
        if (!g_minimized)
            for (int i = 0; i < N_TOGGLES; i++) if (inRect(g_togRc[i], x, y)) { hov = i; break; }
        bool hc = inRect(g_closeRc, x, y);
        bool hm = inRect(g_minRc, x, y);
        bool hk = !g_minimized && inRect(g_keyRc, x, y);
        if (hov != g_hover || hc != g_hoverClose || hm != g_hoverMin || hk != g_hoverKey) {
            g_hover = hov; g_hoverClose = hc; g_hoverMin = hm; g_hoverKey = hk;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1 || g_hoverClose || g_hoverMin || g_hoverKey) {
            g_hover = -1; g_hoverClose = false; g_hoverMin = false; g_hoverKey = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_RBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (inRect(g_sliderRc, x, y)) {         // right-click the slider -> type a CPS value
            if (g_cpsEdit) commitCpsEdit();     // second right-click commits
            else { g_cpsEdit = true; g_cpsBuf[0] = 0; g_dragSlider = false; SetFocus(hwnd); }
            requestRepaint();
        } else if (g_cpsEdit) {                 // right-click elsewhere commits & leaves edit
            commitCpsEdit(); requestRepaint();
        }
        return 0;
    }
    case WM_CHAR: {
        if (!g_cpsEdit) return 0;
        wchar_t ch = (wchar_t)wp;
        size_t n = wcslen(g_cpsBuf);
        if (ch == 13)                                       commitCpsEdit();          // Enter
        else if (ch == 27) { g_cpsEdit = false; g_cpsBuf[0] = 0; }                     // Esc cancels
        else if (ch == 8) { if (n) g_cpsBuf[n - 1] = 0; }                              // Backspace
        else if (((ch >= L'0' && ch <= L'9') || ch == L'.') && n < 5) {
            if (ch != L'.' || !wcschr(g_cpsBuf, L'.')) {    // at most one decimal point
                g_cpsBuf[n] = ch; g_cpsBuf[n + 1] = 0;
            }
        }
        requestRepaint();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_cpsEdit) commitCpsEdit();         // any left-click ends CPS typing
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
        bool onToggle = inRect(g_closeRc, x, y) || inRect(g_minRc, x, y);
        if (!g_minimized)
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

    logf("[ac] running at %.1f CPS (slider 0-30). J or the Autoclicker button arms it. Click the X to unload.\n", g_cps);

    timeBeginPeriod(1);   // 1 ms scheduler resolution -> tight, low-jitter ticks (paired below)
    g_hookThread = CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);
    HANDLE potT  = CreateThread(nullptr, 0, potThread,  nullptr, 0, nullptr);

    using clock = std::chrono::high_resolution_clock;
    auto nextTick  = clock::now();
    auto lastClick = clock::now() - std::chrono::hours(1);  // "long ago" -> first click fires at once
    auto lastRight = lastClick;

    int  lastState = -99;
    bool lastKey = false;
    bool invLeftNext = true;   // in a GUI, alternate left/right clicks so we never double-click
    bool osLeftDown  = false;  // our belief of the OS left-button state during a world hold
    bool prevPhys    = false;  // last tick's g_physDown, to spot a fresh press

    while (g_running) {
        g_gameWindow = findGameWindow();

        // Hotkey rebinding: grab the next key pressed (Esc clears the bind; mouse/End can't bind).
        if (g_rebinding) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                g_hotkey = 0; computeHotkeyName(0);   // unbind: no key toggles the autoclicker
                g_rebinding = false; lastKey = true;
                logf("[ac] hotkey -> unbound\n");
                requestRepaint();
            }
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
        bool keyDown = g_hotkey != 0 && (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
        if (!g_rebinding && keyDown && !lastKey && gameFocused() && !(env && screenOpen(env))) {
            g_enabled = !g_enabled;
            if (!g_enabled) {
                g_physDown = false; g_physRightDown = false;  // drop held state
                g_guiLeftSeized = false; g_guiRightSeized = false;
            }
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

        bool focused = gameForeground();   // only click while Minecraft itself is active, never our GUI
        bool screen  = (g_enabled && focused && env && screenOpen(env));
        g_screenOpen = screen;   // tells the mouse hook whether to seize the left button this tick
        if (!screen && g_guiLeftSeized) {
            g_physDown = false;
            g_guiLeftSeized = false;
        }
        if (!screen && g_guiRightSeized) {
            g_physRightDown = false;
            g_guiRightSeized = false;
        }

        bool holding = g_enabled && g_physDown && focused && !off;
        bool press   = holding && !prevPhys;   // fresh left press this tick
        prevPhys     = holding;

        int state = 0;
        if (holding && screen) {
            // ---- container/menu: the left button is seized, so we inject clean clicks ----
            osLeftDown = false;   // not sustaining a world hold while a screen is up
            // Only autoclick while the user actually holds Shift, so each injected click lands as a
            // shift-click (rapid stack transfer).
            bool shiftHeld = physicalShiftHeld();
            if (!shiftHeld) {
                state = 7;
            }
            // Alternate left/right so the SAME button never double-clicks a slot (which would
            // gather the stack); that makes a double-click impossible at any CPS.
            else if (msSince(lastClick) >= baseGap) {
                // Hold fire only when Splash Pot Move can actually move the hovered pot. If the
                // hotbar is full, the pot path stands down and normal inventory clicks continue.
                bool onPot = g_potMove && (g_potHoldClick || hoveredPotOwnedByPotMove(env));
                if (onPot) {
                    state = 6;
                } else {
                    if (invLeftNext) sendClick(); else sendRightClick();
                    invLeftNext = !invLeftNext;
                    lastClick = now;
                    state = 4;
                }
            } else {
                state = (g_potMove && g_potHoldClick) ? 6 : 4;   // for the status line only
            }
        } else if (holding) {
            // ---- world: the real held button passes straight through, so it digs/attacks on its
            // own. We only drive EXTRA edges on top of that hold: keep it DOWN to mine, UP to guard,
            // or pulse it for combat CPS. Because every combat pulse ends DOWN (held), simply
            // *stopping* the pulses slides the hold straight into mining -- so combat and breaking
            // the ground flow into each other with no release/re-press.
            if (press) osLeftDown = true;   // the real press just put the button down
            bool backTurnSlow = g_backTurn;
            int eval = env ? evalTarget(env, backTurnSlow) : EVAL_ALLOW;
            bool onBlock = (eval == EVAL_BLOCK);
            if (onBlock) {
                // On a block: keep the button DOWN so the real hold digs continuously.
                if (!osLeftDown) { leftEdge(true); osLeftDown = true; }
                state = 8;
            } else {
                // Entity / air: combat. Pulse the button (release->press = one attack) at the CPS.
                clearLeftClickDelay(env);
                double gap = baseGap;
                if (eval == EVAL_BACKTURN) {
                    double bt = 1000.0 / BACKTURN_CPS;   // back turned -> the slower of the two rates
                    if (bt > gap) gap = bt;
                }
                if (msSince(lastClick) >= gap) {
                    leftEdge(false); leftEdge(true);   // one attack, ending DOWN (still held)
                    osLeftDown = true;
                    lastClick = now;
                    clearLeftClickDelay(env);
                }
                state = (eval == EVAL_BACKTURN) ? 5 : (g_canDetectBlocks ? 1 : 3);
            }
        } else {
            if (!focused) {
                g_physDown = false;   // avoid a stuck "down" if focus was lost mid-hold
                g_guiLeftSeized = false;
            }
            osLeftDown = false;                 // not holding -> drop any sustained world state
        }

        // Right clicker: independent of the left logic, gated at the same CPS. Only auto-clicks while
        // a block or bucket is the held item (cached here for the mouse hook); in a GUI it instead
        // honours Shift, matching the inventory right-click behaviour.
        g_holdingPlaceable = (g_enabled && g_rightClicker && focused && env)
                             ? holdingPlaceable(env) : false;
        bool rightActive = false;
        bool rightAllowed = screen ? physicalShiftHeld() : g_holdingPlaceable;
        if (g_enabled && g_rightClicker && g_physRightDown && focused && rightAllowed && !off) {
            if (msSince(lastRight) >= baseGap) { sendRightClick(); lastRight = now; }
            rightActive = true;
        } else if (!focused) {
            g_physRightDown = false;
            g_guiRightSeized = false;
        }

        // Status line. Combine left `state` with the right-click flag so toggling either reprints.
        int combo = state * 2 + (rightActive ? 1 : 0);
        if (combo != lastState) {
            bool leftClicking = (state == 1 || state == 3 || state == 4 || state == 5 || state == 8);
            if      (!g_enabled)             setStatus("disarmed \xc2\xb7 hotkey or button to arm");
            else if (rightActive && leftClicking) setStatus("left + right clicking");
            else if (rightActive)            setStatus("right-clicking \xc2\xb7 hold RMB");
            else if (state == 0) setStatus("armed \xc2\xb7 idle (hold LMB)");
            else if (state == 1) setStatus("clicking \xc2\xb7 entity / air");
            else if (state == 3) setStatus("clicking \xc2\xb7 no block-detect");
            else if (state == 4) setStatus("clicking \xc2\xb7 inventory L/R");
            else if (state == 5) setStatus("back turned \xc2\xb7 6 CPS");
            else if (state == 6) setStatus("pot \xc2\xb7 holding (shift-move)");
            else if (state == 7) setStatus("inventory \xc2\xb7 hold Shift to click");
            else if (state == 8) setStatus("mining block \xc2\xb7 holding LMB");
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
        if (g_itemBlockClass)    env->DeleteGlobalRef(g_itemBlockClass);
        if (g_itemBucketClass)   env->DeleteGlobalRef(g_itemBucketClass);
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
