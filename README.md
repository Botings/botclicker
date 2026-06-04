# injectable

A minimal Windows DLL injector and a demo payload, targeting Minecraft Java (`javaw.exe`).

- **`injector.cpp`** — classic `LoadLibrary` injector via `CreateRemoteThread`.
- **`payload.cpp`** — injected DLL that reports whether a GUI/inventory screen is open,
  using the cursor-grab heuristic (Minecraft releases the OS cursor whenever a screen opens).

## Build

Requires MinGW-w64 g++ (64-bit, to match modern Minecraft's bundled Java).

```powershell
.\build.ps1
```

## Run

1. Launch Minecraft Java and enter a world.
2. `.\injector.exe`  (run the terminal as administrator if `OpenProcess` fails).
3. A console reports inventory/GUI state. Open your inventory (E) to see it flip.
4. Press **END** to cleanly unload the DLL.

## Limitation

The cursor heuristic detects *any* open screen (inventory, chat, pause, chests) — it can't
single out the inventory specifically. True inventory-only detection needs a JVMTI agent or a
Forge mod that reads `Minecraft.currentScreen`.
