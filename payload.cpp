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
#include <mmsystem.h>

#pragma comment(lib, "gdiplus.lib")

static HMODULE       g_self      = nullptr;
static volatile bool g_running   = true;

static JavaVM*       g_jvm       = nullptr;
static jvmtiEnv*     g_jvmti     = nullptr;

static jclass        g_mcClass    = nullptr;
static jobject       g_mcInstance = nullptr;
static jfieldID      g_fObjMouseOver = nullptr;
static jfieldID      g_fCurrentScreen = nullptr;
static jfieldID      g_fLeftClickCounter = nullptr;
static jclass        g_mopClass   = nullptr;
static jfieldID      g_fTypeOfHit = nullptr;
static jmethodID     g_midOrdinal = nullptr;
static bool          g_canDetectBlocks = false;

static jfieldID      g_fThePlayer = nullptr;

static jfieldID      g_fInventory = nullptr;
static jfieldID      g_fMainInv   = nullptr;

static jfieldID      g_fCurrentItem    = nullptr;
static jmethodID     g_midGetCurItem   = nullptr;
static jmethodID     g_midGetItem      = nullptr;
static jclass        g_itemBlockClass  = nullptr;
static jclass        g_itemBucketClass = nullptr;
static bool          g_canDetectHeld   = false;

static volatile HWND g_gameWindow = nullptr;
static volatile bool g_physDown      = false;
static volatile bool g_physRightDown = false;

static volatile bool g_screenOpen = false;
static volatile bool g_guiLeftSeized = false;
static volatile bool g_guiRightSeized = false;
static volatile bool g_holdingPlaceable = false;
static HHOOK         g_mouseHook   = nullptr;
static HANDLE        g_hookThread  = nullptr;
static DWORD         g_hookThreadId = 0;

static volatile bool g_enabled     = false;
static volatile bool g_rightClicker= false;
static volatile bool g_alwaysLeft  = false;

static volatile int  g_hotkey      = 'J';
static wchar_t       g_hotkeyName[32] = L"J";
static volatile bool g_rebinding   = false;

static volatile double g_cps     = 15.8;
static const double  CPS_MIN     = 0.0;
static const double  CPS_MAX     = 30.0;
static const double  CPS_OFF_EPS = 0.1;
static const ULONG_PTR INPUT_TAG = 0x42434C4B;

enum { EVAL_ALLOW = 0, EVAL_BLOCK = 1 };

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

static void computeHotkeyName(int vk) {
    if (vk == 0) { wcscpy_s(g_hotkeyName, L"None"); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP:   case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END:  case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= (1L << 24); break;
    }
    wchar_t buf[32] = {};
    if (sc != 0 && GetKeyNameTextW(lp, buf, 31) > 0) wcsncpy_s(g_hotkeyName, buf, _TRUNCATE);
    else                                             swprintf_s(g_hotkeyName, L"0x%02X", (unsigned)vk);
}

struct FindData { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* fd = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == fd->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        char t[256];

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

    return g && (fg == g || fg == g_hwnd);
}

static inline bool gameForeground() {
    HWND g = g_gameWindow;
    return g && GetForegroundWindow() == g;
}

static inline bool physicalShiftHeld() {
    return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
}

static LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        bool injected = (m->flags & LLMHF_INJECTED) != 0;

        if (!injected && g_enabled && g_gameWindow && GetForegroundWindow() == g_gameWindow) {
            bool screen = g_screenOpen;
            bool shift = physicalShiftHeld();

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
                    bool swallow = g_physRightDown;
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

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, g_self, 0);
    if (!g_mouseHook) { logf("[ac] WARNING: mouse hook failed (%lu)\n", GetLastError()); return 1; }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {  }
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    return 0;
}

static INPUT mouseButtonInput(DWORD flags) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.dwExtraInfo = INPUT_TAG;
    return in;
}

static void sendClick() {
    INPUT in[2] = {};
    in[0] = mouseButtonInput(MOUSEEVENTF_LEFTDOWN);
    in[1] = mouseButtonInput(MOUSEEVENTF_LEFTUP);
    SendInput(2, in, sizeof(INPUT));
}

static void sendRightClick() {
    INPUT in[2] = {};
    in[0] = mouseButtonInput(MOUSEEVENTF_RIGHTDOWN);
    in[1] = mouseButtonInput(MOUSEEVENTF_RIGHTUP);
    SendInput(2, in, sizeof(INPUT));
}

static void leftEdge(bool down) {
    INPUT in = mouseButtonInput(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof(INPUT));
}

static void pulseHeldLeftClick() {
    INPUT in[2] = {};
    in[0] = mouseButtonInput(MOUSEEVENTF_LEFTUP);
    in[1] = mouseButtonInput(MOUSEEVENTF_LEFTDOWN);
    SendInput(2, in, sizeof(INPUT));
}

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

static jmethodID findMethod(JNIEnv* env, jclass klass, const char* mcp, const char* srg, const char* sig) {
    jmethodID m = env->GetMethodID(klass, mcp, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!m && srg) {
        m = env->GetMethodID(klass, srg, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return m;
}

static bool ensureHeldRefs(JNIEnv* env);

static bool setupJvm(JNIEnv* env) {

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

    jclass enumC = env->FindClass("java/lang/Enum");
    if (enumC) { g_midOrdinal = env->GetMethodID(enumC, "ordinal", "()I"); env->DeleteLocalRef(enumC); }
    if (env->ExceptionCheck()) env->ExceptionClear();

    g_canDetectBlocks = (g_mcInstance && g_fObjMouseOver && g_midOrdinal);
    logf("[ac] field resolution: theMinecraft=%s objectMouseOver=%s currentScreen=%s\n",
         g_mcInstance ? "ok" : "MISS", g_fObjMouseOver ? "ok" : "MISS", g_fCurrentScreen ? "ok" : "MISS");
    if (!g_canDetectBlocks)
        logf("[ac] WARNING: block detection unavailable -> will click everywhere (incl. blocks).\n");
    logf("[ac] vanilla hit-delay reset: %s\n", g_fLeftClickCounter ? "ok" : "MISS");

    g_fThePlayer = findField(g_mcClass, "thePlayer", "field_71439_g", nullptr);

    jclass playerClass = findLoadedClass(env, "Lnet/minecraft/entity/player/EntityPlayer;");
    jclass invClass    = findLoadedClass(env, "Lnet/minecraft/entity/player/InventoryPlayer;");
    if (playerClass) { g_fInventory = findField(playerClass, "inventory", "field_71071_by", nullptr); env->DeleteGlobalRef(playerClass); }
    if (invClass)    { g_fMainInv   = findField(invClass,    "mainInventory", "field_70462_a", nullptr); env->DeleteGlobalRef(invClass); }

    ensureHeldRefs(env);
    logf("[ac] held-item detection: %s\n", g_canDetectHeld ? "ok" : "MISS -> right-clicker stays idle");

    _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE,
                "blocks %s \xc2\xb7 delay %s \xc2\xb7 held %s",
                g_canDetectBlocks ? "ok" : "off", g_fLeftClickCounter ? "ok" : "off",
                g_canDetectHeld ? "ok" : "off");
    requestRepaint();
    return true;
}

static void clearLeftClickDelay(JNIEnv* env) {
    if (!env || !g_mcInstance || !g_fLeftClickCounter) return;
    env->SetIntField(g_mcInstance, g_fLeftClickCounter, 0);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

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

    bool canFetch = (g_fMainInv && g_fCurrentItem) || g_midGetCurItem;
    g_canDetectHeld = g_fThePlayer && g_fInventory && g_midGetItem &&
                      g_itemBlockClass && g_itemBucketClass && canFetch;
    return g_canDetectHeld;
}

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

static bool holdingPlaceable(JNIEnv* env) {
    if (!ensureHeldRefs(env)) return false;
    jobject me = env->GetObjectField(g_mcInstance, g_fThePlayer);
    if (!me) return false;
    jobject inv = env->GetObjectField(me, g_fInventory);
    env->DeleteLocalRef(me);
    if (!inv) return false;
    jobject stack = heldStack(env, inv);
    env->DeleteLocalRef(inv);
    if (!stack) return false;
    jobject item = env->CallObjectMethod(stack, g_midGetItem);
    env->DeleteLocalRef(stack);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!item) return false;
    bool placeable = env->IsInstanceOf(item, g_itemBlockClass) ||
                     env->IsInstanceOf(item, g_itemBucketClass);
    env->DeleteLocalRef(item);
    return placeable;
}

static bool screenOpen(JNIEnv* env) {
    if (!g_canDetectBlocks || !g_fCurrentScreen || !g_mcInstance) return false;
    jobject scr = env->GetObjectField(g_mcInstance, g_fCurrentScreen);
    bool open = (scr != nullptr);
    if (scr) env->DeleteLocalRef(scr);
    return open;
}

static int evalTarget(JNIEnv* env) {
    if (!g_canDetectBlocks) return EVAL_ALLOW;

    jobject mop = env->GetObjectField(g_mcInstance, g_fObjMouseOver);
    if (!mop) return EVAL_ALLOW;

    if (!g_mopClass) {
        jclass c = env->GetObjectClass(mop);
        g_mopClass = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
        g_fTypeOfHit = findField(g_mopClass, "typeOfHit", "field_72313_a", nullptr);
    }

    int result = EVAL_ALLOW;
    if (g_fTypeOfHit) {
        jobject t = env->GetObjectField(mop, g_fTypeOfHit);
        if (t) {
            jint ord = env->CallIntMethod(t, g_midOrdinal);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(t);
            if (ord == 1) result = EVAL_BLOCK;
        }
    }
    env->DeleteLocalRef(mop);
    return result;
}

using namespace Gdiplus;

static const int   WIN_W = 360, WIN_H = 586;
static const int   TOG_X = 22, TOG_W = WIN_W - 44, TOG_H = 54, TOG_GAP = 12, TOG_Y0 = 120;
static const int   N_TOGGLES = 3;

struct ToggleDef { const wchar_t* label; const wchar_t* hint; volatile bool* state; };
static ToggleDef g_toggles[N_TOGGLES] = {
    { L"Autoclicker",      L"left-click (hotkey)", &g_enabled     },
    { L"Right Clicker",    L"blocks & buckets only", &g_rightClicker},
    { L"Always Left Click",L"click nonstop, no hold", &g_alwaysLeft  },
};
static RECT g_togRc[N_TOGGLES];
static RECT g_sliderRc;
static bool g_dragSlider = false;
static bool g_cpsEdit    = false;
static wchar_t g_cpsBuf[16] = L"";
static RECT g_keyRc;
static RECT g_closeRc = { WIN_W - 40, 14, WIN_W - 16, 38 };
static RECT g_minRc   = { WIN_W - 72, 14, WIN_W - 48, 38 };
static const int COLLAPSED_H = 64;
static bool g_minimized = false;
static int  g_hover = -1;
static bool g_hoverClose = false;
static bool g_hoverMin   = false;
static bool g_hoverKey   = false;
static ULONG_PTR g_gdipToken = 0;

static void layoutToggles() {
    for (int i = 0; i < N_TOGGLES; i++) {
        int top = TOG_Y0 + i * (TOG_H + TOG_GAP);
        g_togRc[i] = { TOG_X, top, TOG_X + TOG_W, top + TOG_H };
    }
    int st = TOG_Y0 + N_TOGGLES * (TOG_H + TOG_GAP);
    g_sliderRc = { TOG_X, st, TOG_X + TOG_W, st + TOG_H };
    int kt = st + (TOG_H + TOG_GAP);
    g_keyRc = { TOG_X, kt, TOG_X + TOG_W, kt + TOG_H };
}
static inline bool inRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

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
    g_cps = floor(v * 10.0 + 0.5) / 10.0;
}

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

        RectF full(0, 0, (REAL)W, (REAL)H);
        LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)H), C(15, 21, 40), C(7, 10, 22));
        g.FillRectangle(&bg, full);

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

        {
            SolidBrush muted(C(120, 135, 170));
            wchar_t sub[48];
            if (g_cps < CPS_OFF_EPS) swprintf_s(sub, L"Hold LMB  \xb7  paused");
            else                     swprintf_s(sub, L"Hold LMB  \xb7  %.1f CPS", g_cps);
            g.DrawString(sub, -1, fSub, RectF(0, 74, (REAL)W, 18), &sfC, &muted);
        }

        {
            Pen ln(C(38, 50, 84), 1.0f);
            g.DrawLine(&ln, 28.0f, 104.0f, (REAL)W - 28, 104.0f);
        }

        {
            RectF cr((REAL)g_closeRc.left, (REAL)g_closeRc.top,
                     (REAL)(g_closeRc.right - g_closeRc.left), (REAL)(g_closeRc.bottom - g_closeRc.top));
            fillRound(g, g_hoverClose ? C(210, 64, 64) : C(34, 44, 72), cr, 7.0f);
            Pen xp(g_hoverClose ? C(255, 235, 235) : C(150, 165, 200), 1.8f);
            REAL pad = 7;
            g.DrawLine(&xp, cr.X + pad, cr.Y + pad, cr.X + cr.Width - pad, cr.Y + cr.Height - pad);
            g.DrawLine(&xp, cr.X + cr.Width - pad, cr.Y + pad, cr.X + pad, cr.Y + cr.Height - pad);
        }

        {
            RectF mr((REAL)g_minRc.left, (REAL)g_minRc.top,
                     (REAL)(g_minRc.right - g_minRc.left), (REAL)(g_minRc.bottom - g_minRc.top));
            fillRound(g, g_hoverMin ? C(54, 86, 150) : C(34, 44, 72), mr, 7.0f);
            Pen mp(g_hoverMin ? C(235, 242, 255) : C(150, 165, 200), 1.8f);
            REAL pad = 7;
            if (g_minimized) {
                g.DrawRectangle(&mp, mr.X + pad, mr.Y + pad, mr.Width - 2 * pad, mr.Height - 2 * pad);
            } else {
                g.DrawLine(&mp, mr.X + pad, mr.Y + mr.Height - pad, mr.X + mr.Width - pad, mr.Y + mr.Height - pad);
            }
        }

        wchar_t wbuf[128];
        if (g_minimized) {

        } else {

        for (int i = 0; i < N_TOGGLES; i++) {
            const RECT& rc = g_togRc[i];
            bool on = *g_toggles[i].state;
            RectF card((REAL)rc.left, (REAL)rc.top, (REAL)TOG_W, (REAL)TOG_H);

            Color cardCol = (i == g_hover) ? C(30, 41, 70) : C(21, 29, 52);
            fillRound(g, cardCol, card, 13.0f);
            strokeRound(g, on ? C(46, 120, 80) : C(40, 52, 86), 1.2f, card, 13.0f);

            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, on ? C(54, 211, 130) : C(228, 78, 78), strip, 2.0f);
            }

            SolidBrush txt(C(228, 235, 250)), hint(C(118, 132, 168));
            g.DrawString(g_toggles[i].label, -1, fLabel,
                         RectF(card.X + 24, card.Y + 9, TOG_W - 120, 20), &sfL, &txt);
            g.DrawString(g_toggles[i].hint, -1, fHint,
                         RectF(card.X + 24, card.Y + 30, TOG_W - 120, 16), &sfL, &hint);

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

        {
            RectF card((REAL)g_sliderRc.left, (REAL)g_sliderRc.top, (REAL)TOG_W, (REAL)TOG_H);
            fillRound(g, (g_dragSlider || g_cpsEdit) ? C(30, 41, 70) : C(21, 29, 52), card, 13.0f);
            strokeRound(g, g_cpsEdit ? C(214, 168, 40) : (g_dragSlider ? C(96, 165, 255) : C(40, 52, 86)), 1.2f, card, 13.0f);

            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, C(96, 165, 255), strip, 2.0f);
            }

            SolidBrush txt(C(228, 235, 250));
            g.DrawString(L"Click Speed", -1, fLabel,
                         RectF(card.X + 24, card.Y + 6, TOG_W - 120, 20), &sfL, &txt);
            {
                wchar_t vb[40];
                if (g_cpsEdit)               swprintf_s(vb, L"%s_ CPS", g_cpsBuf);
                else if (g_cps < CPS_OFF_EPS) swprintf_s(vb, L"off");
                else                         swprintf_s(vb, L"%.1f CPS", g_cps);
                StringFormat sfR; sfR.SetAlignment(StringAlignmentFar); sfR.SetLineAlignment(StringAlignmentCenter);
                SolidBrush vcol(g_cpsEdit ? C(245, 205, 80) : C(96, 165, 255));
                g.DrawString(vb, -1, fPill, RectF(card.X, card.Y + 6, card.Width - 16, 20), &sfR, &vcol);
            }

            double tx0, tx1, ty; sliderTrack(&tx0, &tx1, &ty);
            REAL trH = 5.0f;
            fillRound(g, C(40, 52, 86), RectF((REAL)tx0, (REAL)ty - trH / 2, (REAL)(tx1 - tx0), trH), trH / 2);
            double kx = cpsToX(tx0, tx1);
            if (kx > tx0 + 1)
                fillRound(g, C(54, 140, 255), RectF((REAL)tx0, (REAL)ty - trH / 2, (REAL)(kx - tx0), trH), trH / 2);

            REAL kr = g_dragSlider ? 10.0f : 8.0f;
            SolidBrush kb(C(234, 240, 252));
            g.FillEllipse(&kb, (REAL)kx - kr, (REAL)ty - kr, kr * 2, kr * 2);
            Pen kp(C(54, 140, 255), 2.0f);
            g.DrawEllipse(&kp, (REAL)kx - kr, (REAL)ty - kr, kr * 2, kr * 2);
        }

        {
            RectF card((REAL)g_keyRc.left, (REAL)g_keyRc.top, (REAL)TOG_W, (REAL)TOG_H);
            fillRound(g, g_hoverKey ? C(30, 41, 70) : C(21, 29, 52), card, 13.0f);
            strokeRound(g, g_rebinding ? C(214, 168, 40) : C(40, 52, 86), 1.2f, card, 13.0f);

            {
                RectF strip(card.X + 8, card.Y + 12, 4.0f, card.Height - 24);
                fillRound(g, C(96, 165, 255), strip, 2.0f);
            }

            SolidBrush txt(C(228, 235, 250)), hint(C(118, 132, 168));
            g.DrawString(L"Toggle Hotkey", -1, fLabel,
                         RectF(card.X + 24, card.Y + 9, TOG_W - 120, 20), &sfL, &txt);
            g.DrawString(g_rebinding ? L"press any key  \xb7  Esc clears bind" : L"click, then press a key", -1, fHint,
                         RectF(card.X + 24, card.Y + 30, TOG_W - 110, 16), &sfL, &hint);

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

        {
            RectF st(22, (REAL)(g_keyRc.bottom + 12), (REAL)(WIN_W - 44), 44);
            fillRound(g, C(16, 22, 42), st, 12.0f);
            strokeRound(g, C(36, 48, 80), 1.0f, st, 12.0f);

            bool live = g_enabled;
            Color dot = live ? C(54, 211, 130) : C(150, 160, 190);
            SolidBrush db(dot);
            g.FillEllipse(&db, st.X + 16, st.Y + st.Height / 2 - 5, 10.0f, 10.0f);

            wideOf(g_status, wbuf, 128);
            SolidBrush stx(C(206, 216, 238));
            g.DrawString(wbuf, -1, fStatus, RectF(st.X + 34, st.Y, st.Width - 44, st.Height), &sfL, &stx);
        }
        }

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

static void applyMinimizedState(HWND hwnd) {
    int h = g_minimized ? COLLAPSED_H : WIN_H;
    SetWindowPos(hwnd, nullptr, 0, 0, WIN_W, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN rgn = CreateRoundRectRgn(0, 0, WIN_W + 1, h + 1, 22, 22);
    SetWindowRgn(hwnd, rgn, TRUE);
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
        SetTimer(hwnd, 1, 140, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_dragSlider) {
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
        if (inRect(g_sliderRc, x, y)) {
            if (g_cpsEdit) commitCpsEdit();
            else { g_cpsEdit = true; g_cpsBuf[0] = 0; g_dragSlider = false; SetFocus(hwnd); }
            requestRepaint();
        } else if (g_cpsEdit) {
            commitCpsEdit(); requestRepaint();
        }
        return 0;
    }
    case WM_CHAR: {
        if (!g_cpsEdit) return 0;
        wchar_t ch = (wchar_t)wp;
        size_t n = wcslen(g_cpsBuf);
        if (ch == 13)                                       commitCpsEdit();
        else if (ch == 27) { g_cpsEdit = false; g_cpsBuf[0] = 0; }
        else if (ch == 8) { if (n) g_cpsBuf[n - 1] = 0; }
        else if (((ch >= L'0' && ch <= L'9') || ch == L'.') && n < 5) {
            if (ch != L'.' || !wcschr(g_cpsBuf, L'.')) {
                g_cpsBuf[n] = ch; g_cpsBuf[n + 1] = 0;
            }
        }
        requestRepaint();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_cpsEdit) commitCpsEdit();
        if (inRect(g_sliderRc, x, y)) {
            g_dragSlider = true;
            SetCapture(hwnd);
            double tx0, tx1, ty; sliderTrack(&tx0, &tx1, &ty);
            setCpsFromX((double)x, tx0, tx1);
            requestRepaint();
            return 0;
        }
        if (inRect(g_keyRc, x, y)) {
            g_rebinding = !g_rebinding;
            requestRepaint();
            return 0;
        }
        bool onToggle = inRect(g_closeRc, x, y) || inRect(g_minRc, x, y);
        if (!g_minimized)
            for (int i = 0; i < N_TOGGLES; i++) if (inRect(g_togRc[i], x, y)) onToggle = true;
        if (onToggle) { toggleAt(x, y); return 0; }
        if (y < 104) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragSlider) { g_dragSlider = false; ReleaseCapture(); requestRepaint(); }
        return 0;
    case WM_CLOSE:
        g_running = false;
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

    HRGN rgn = CreateRoundRectRgn(0, 0, WIN_W + 1, WIN_H + 1, 22, 22);
    SetWindowRgn(hwnd, rgn, TRUE);
    SetLayeredWindowAttributes(hwnd, 0, 247, LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    g_hwnd = nullptr;

    UnregisterClassW(L"BotClickerWnd", (HINSTANCE)g_self);
    GdiplusShutdown(g_gdipToken);
    return 0;
}

static void run() {
    logf("[ac] attached to pid %lu. opening UI + resolving JVM...\n", GetCurrentProcessId());

    HANDLE guiT = CreateThread(nullptr, 0, guiThread, nullptr, 0, nullptr);

    JNIEnv* env = nullptr;
    if (g_jvm == nullptr) {

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

    timeBeginPeriod(1);
    g_hookThread = CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto stale = start - std::chrono::hours(1);
    auto nextLeftClick = start;
    auto nextRightClick = start;
    auto lastWindowRefresh = stale;
    auto lastScreenPoll    = stale;
    auto lastTargetPoll    = stale;
    auto lastHeldPoll      = stale;
    auto lastDelayClear    = stale;

    int  lastState = -99;
    bool lastKey = false;
    bool invLeftNext = true;
    bool osLeftDown  = false;
    bool prevPhys    = false;
    bool cachedScreenOpen = false;
    int  cachedTargetEval = EVAL_ALLOW;
    bool cachedPlaceable  = false;

    while (g_running) {
        auto now = clock::now();
        auto msSince = [&](const clock::time_point& t) {
            return std::chrono::duration<double, std::milli>(now - t).count();
        };

        HWND game = g_gameWindow;
        bool refreshWindow = msSince(lastWindowRefresh) >= 250.0;
        bool staleWindow = game && !IsWindow(game);
        if (refreshWindow || staleWindow) {
            g_gameWindow = findGameWindow();
            lastWindowRefresh = now;
        }

        bool focusedAny = gameFocused();
        bool focused    = gameForeground();

        if (env && focusedAny) {
            if (msSince(lastScreenPoll) >= 20.0) {
                cachedScreenOpen = screenOpen(env);
                lastScreenPoll = now;
            }
        } else {
            cachedScreenOpen = false;
            lastScreenPoll = now;
        }

        if (g_rebinding) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                g_hotkey = 0; computeHotkeyName(0);
                g_rebinding = false; lastKey = true;
                logf("[ac] hotkey -> unbound\n");
                requestRepaint();
            }
            else for (int vk = 0x08; vk <= 0xFE; vk++) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                    vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == VK_END) continue;
                if (GetAsyncKeyState(vk) & 0x8000) {
                    g_hotkey = vk; computeHotkeyName(vk);
                    g_rebinding = false; lastKey = true;
                    logf("[ac] hotkey -> %ls (0x%02X)\n", g_hotkeyName, vk);
                    requestRepaint();
                    break;
                }
            }
        }

        bool keyDown = g_hotkey != 0 && (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
        if (!g_rebinding && keyDown && !lastKey && focusedAny && !cachedScreenOpen) {
            g_enabled = !g_enabled;
            if (!g_enabled) {
                g_physDown = false; g_physRightDown = false;
                g_guiLeftSeized = false; g_guiRightSeized = false;
            }
            logf("[ac] %s\n", g_enabled ? "ENABLED" : "DISABLED");
            requestRepaint();
            lastState = -99;
        }
        lastKey = keyDown;

        double cps = g_cps;
        bool   off = (cps < CPS_OFF_EPS);
        double baseGap = off ? 1e9 : (1000.0 / cps);
        auto clickGap = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(baseGap));
        auto inventoryGap = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(baseGap * 0.5));
        auto scheduleNext = [&](clock::time_point& deadline, clock::duration gap) {
            deadline += gap;
            if (deadline <= now) deadline = now + gap;
        };

        bool screen  = (g_enabled && focused && cachedScreenOpen);
        g_screenOpen = screen;
        if (!screen && g_guiLeftSeized) {
            g_physDown = false;
            g_guiLeftSeized = false;
        }
        if (!screen && g_guiRightSeized) {
            g_physRightDown = false;
            g_guiRightSeized = false;
        }

        bool holding = g_enabled && g_physDown && focused && !off;
        bool press   = holding && !prevPhys;
        prevPhys     = holding;

        bool alwaysLeft = g_alwaysLeft && g_enabled && focused && !off;

        int state = 0;
        if (alwaysLeft) {
            osLeftDown = false;
            if (now >= nextLeftClick) {
                sendClick();
                scheduleNext(nextLeftClick, clickGap);
            }
            state = 9;
        } else if (holding && screen) {

            osLeftDown = false;

            bool shiftHeld = physicalShiftHeld();
            if (!shiftHeld) {
                state = 7;
            }

            else if (now >= nextLeftClick) {
                if (invLeftNext) sendClick(); else sendRightClick();
                invLeftNext = !invLeftNext;
                scheduleNext(nextLeftClick, inventoryGap);
                state = 4;
            } else {
                state = 4;
            }
        } else if (holding) {

            if (press) osLeftDown = true;
            if (press || msSince(lastTargetPoll) >= 5.0) {
                cachedTargetEval = env ? evalTarget(env) : EVAL_ALLOW;
                lastTargetPoll = now;
            }
            int eval = cachedTargetEval;
            bool onBlock = (eval == EVAL_BLOCK);
            if (onBlock) {

                if (!osLeftDown) { leftEdge(true); osLeftDown = true; }
                state = 8;
            } else {

                if (msSince(lastDelayClear) >= 15.0) {
                    clearLeftClickDelay(env);
                    lastDelayClear = now;
                }
                if (now >= nextLeftClick) {
                    clearLeftClickDelay(env);
                    pulseHeldLeftClick();
                    osLeftDown = true;
                    scheduleNext(nextLeftClick, clickGap);
                    clearLeftClickDelay(env);
                    lastDelayClear = now;
                }
                state = g_canDetectBlocks ? 1 : 3;
            }
        } else {
            if (!focused) {
                g_physDown = false;
                g_guiLeftSeized = false;
            }
            osLeftDown = false;
            cachedTargetEval = EVAL_ALLOW;
            lastTargetPoll = now;
            nextLeftClick = now;
        }

        if (g_enabled && g_rightClicker && focused && env) {
            if (msSince(lastHeldPoll) >= 50.0) {
                cachedPlaceable = holdingPlaceable(env);
                lastHeldPoll = now;
            }
        } else {
            cachedPlaceable = false;
            lastHeldPoll = now;
        }
        g_holdingPlaceable = cachedPlaceable;
        bool rightActive = false;
        bool rightAllowed = screen ? physicalShiftHeld() : g_holdingPlaceable;
        if (g_enabled && g_rightClicker && g_physRightDown && focused && rightAllowed && !off) {
            if (now >= nextRightClick) {
                sendRightClick();
                scheduleNext(nextRightClick, clickGap);
            }
            rightActive = true;
        } else if (!focused) {
            g_physRightDown = false;
            g_guiRightSeized = false;
            nextRightClick = now;
        } else {
            nextRightClick = now;
        }

        int combo = state * 2 + (rightActive ? 1 : 0);
        if (combo != lastState) {
            bool leftClicking = (state == 1 || state == 3 || state == 4 || state == 8 || state == 9);
            if      (!g_enabled)             setStatus("disarmed \xc2\xb7 hotkey or button to arm");
            else if (rightActive && leftClicking) setStatus("left + right clicking");
            else if (rightActive)            setStatus("right-clicking \xc2\xb7 hold RMB");
            else if (state == 0) setStatus("armed \xc2\xb7 idle (hold LMB)");
            else if (state == 1) setStatus("clicking \xc2\xb7 entity / air");
            else if (state == 3) setStatus("clicking \xc2\xb7 no block-detect");
            else if (state == 4) setStatus("clicking \xc2\xb7 inventory L/R");
            else if (state == 7) setStatus("inventory \xc2\xb7 hold Shift to click");
            else if (state == 8) setStatus("mining block \xc2\xb7 holding LMB");
            else if (state == 9) setStatus("always left-clicking");
            lastState = combo;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    timeEndPeriod(1);
    g_running = false;

    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) { WaitForSingleObject(g_hookThread, 4000); CloseHandle(g_hookThread); }

    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (guiT) { WaitForSingleObject(guiT, 4000); CloseHandle(guiT); }

    if (env) {
        if (g_mcInstance)        env->DeleteGlobalRef(g_mcInstance);
        if (g_mcClass)           env->DeleteGlobalRef(g_mcClass);
        if (g_mopClass)          env->DeleteGlobalRef(g_mopClass);
        if (g_itemBlockClass)    env->DeleteGlobalRef(g_itemBlockClass);
        if (g_itemBucketClass)   env->DeleteGlobalRef(g_itemBucketClass);
    }
    if (env && g_jvm) g_jvm->DetachCurrentThread();

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
