#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>

static DWORD findPid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv) {
    const char* procName = (argc > 1) ? argv[1] : "javaw.exe";
    std::string dllArg   = (argc > 2) ? argv[2] : "payload.dll";

    char dllPath[MAX_PATH];
    if (!GetFullPathNameA(dllArg.c_str(), MAX_PATH, dllPath, nullptr)) {
        printf("[injector] could not resolve dll path: %s\n", dllArg.c_str());
        return 1;
    }
    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[injector] dll not found: %s\n", dllPath);
        return 1;
    }

    DWORD pid = findPid(procName);
    if (!pid) {
        printf("[injector] process not found: %s  (is Minecraft running?)\n", procName);
        return 1;
    }
    printf("[injector] target %s  pid %lu\n", procName, pid);
    printf("[injector] dll    %s\n", dllPath);

    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        printf("[injector] OpenProcess failed: %lu  (try running as administrator)\n", GetLastError());
        return 1;
    }

    SIZE_T sz = dllArg.size() ? (strlen(dllPath) + 1) : 1;
    LPVOID remote = VirtualAllocEx(proc, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        printf("[injector] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }

    if (!WriteProcessMemory(proc, remote, dllPath, sz, nullptr)) {
        printf("[injector] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryA"));
    if (!loadLib) {
        printf("[injector] GetProcAddress(LoadLibraryA) failed\n");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) {
        printf("[injector] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    printf("[injector] injecting...\n");
    WaitForSingleObject(th, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);

    if (exitCode == 0)
        printf("[injector] LoadLibraryA returned NULL -- injection failed "
               "(arch mismatch, or the DLL's dependencies couldn't load).\n");
    else
        printf("[injector] success. remote module base (low 32 bits): 0x%08lX\n", exitCode);

    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return 0;
}
