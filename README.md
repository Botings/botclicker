# injectable

A minimal Windows DLL injector workspace targeting Minecraft Java (`javaw.exe`).

- **`injector.cpp`** — classic `LoadLibrary` injector via `CreateRemoteThread`.
- **`payload.cpp`** — a JVMTI/JNI agent: a left-click autoclicker for combat and mining.

## What the agent does

While Minecraft is the foreground window, holding **left-click** fires clicks at **15.8 CPS**:

- A low-level mouse hook layers synthesized click-edges on top of your real held button:
  in the world the hold passes through, and in a container GUI it seizes Shift+LMB to
  inject clean clicks.
- It reads `Minecraft.objectMouseOver.typeOfHit` over JVMTI/JNI:
  - aimed at a **block** → holds LMB down so it digs continuously,
  - **air / entity** → pulses clicks (attack swings) at the set CPS,
  - a **GUI is open** (`currentScreen != null`, e.g. your inventory) → clicks through.
- Field names are tried as MCP (dev) then SRG (prod), so the same DLL works in a Gradle
  `runClient` session and a shipped Forge client. If neither resolves it degrades to
  "always click" and prints a warning.

Press **END** to unload cleanly.

## Build

Requires MinGW-w64 g++ (64-bit) and a JDK with JNI/JVMTI headers. Defaults to
`C:/Program Files/Java/jdk1.8.0_201`; override with `$env:JDK_HOME`.

```powershell
.\build.ps1
```

## Run

1. Launch Minecraft Java (1.8.9 Forge) and enter a world.
2. BotClicker: `.\injector.exe`  (run the terminal as administrator if `OpenProcess` fails).
3. The BotClicker overlay shows agent state (idle / autoclicking / aimed-at-block).
4. Press **END** to cleanly unload BotClicker.

## Notes / assumptions

- This workspace needs MinGW-w64 g++ available on PATH, in a common MSYS2 location,
  or via `$env:CXX` before the DLLs can be rebuilt.
- Field names target **1.8.9 `stable_22`** mappings. Other versions need their own MCP/SRG
  names added in `payload.cpp` (`findField` calls).
- Arch must match the JVM (64-bit here). Anti-cheats that monitor `SetWindowsHookEx` /
  `CreateRemoteThread` may flag this.
