#include "../native/control_center/client_identity.hpp"
#include <array>
#include <cstdio>
#include <string>
#include <vector>

static bool Fixture(const std::wstring& dir, WORD machine, bool valid = true) {
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW((dir + L"\\wow.exe").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::array<unsigned char, 512> data{};
    data[0] = valid ? 'M' : 'X'; data[1] = 'Z'; data[0x3c] = 0x80;
    data[0x80] = 'P'; data[0x81] = 'E'; data[0x84] = static_cast<unsigned char>(machine & 0xff);
    data[0x85] = static_cast<unsigned char>(machine >> 8);
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(h); return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--child") {
        HANDLE signal = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!signal) return 90;
        const auto result = WaitForSingleObject(signal, 20000);
        CloseHandle(signal);
        return result == WAIT_OBJECT_0 ? 0 : 91;
    }
    if (argc != 2) return 1;
    const std::wstring base = argv[1], x86 = base + L"\\client-x86", x64 = base + L"\\client-x64";
    const std::wstring broken = base + L"\\broken", protectedRoot = base + L"\\reference";
    const std::wstring sibling = base + L"\\reference-other";
    if (!Fixture(x86, IMAGE_FILE_MACHINE_I386) || !Fixture(x64, IMAGE_FILE_MACHINE_AMD64) ||
        !Fixture(broken, IMAGE_FILE_MACHINE_I386, false) || !Fixture(protectedRoot, IMAGE_FILE_MACHINE_I386) ||
        !Fixture(sibling, IMAGE_FILE_MACHINE_I386)) return 2;
    using control_center::ClientError;
    const std::vector<std::wstring> protectedRoots{protectedRoot};
    if (!control_center::ValidateClient(x86, protectedRoots)) return 3;
    if (control_center::ValidateClient(x64, protectedRoots).error != ClientError::NotX86) return 4;
    if (control_center::ValidateClient(broken, protectedRoots).error != ClientError::InvalidPe) return 5;
    if (control_center::ValidateClient(protectedRoot, protectedRoots).error != ClientError::ProtectedRoot) return 6;
    if (control_center::ValidateClient(protectedRoot + L".", protectedRoots).error != ClientError::InvalidRoot) return 19;
    if (control_center::ValidateClient(protectedRoot + L" ", protectedRoots).error != ClientError::InvalidRoot) return 20;
    if (control_center::ValidateClient(protectedRoot + L"\\..\\reference-other", protectedRoots).error != ClientError::InvalidRoot) return 21;
    if (!control_center::ValidateClient(sibling, protectedRoots)) return 7;
    if (control_center::ValidateClient(base + L"\\link", protectedRoots).error != ClientError::InvalidRoot) return 8;
    if (control_center::ValidateClient(base + L"\\real", protectedRoots).error != ClientError::MissingOrUnreadable) return 9;
    if (control_center::ValidateClient(L"relative", protectedRoots).error != ClientError::InvalidRoot) return 10;
    if (control_center::ValidateClient(x86, {L"bad-root"}).error != ClientError::InvalidRoot) return 11;
    if (!control_center::IsBlockingProcessName(L"WoWcLaSsIc.ExE") ||
        !control_center::IsBlockingProcessName(L"patchmenu.exe") ||
        !control_center::IsBlockingProcessName(L"AIO DLSS5 32-bit Wrapper.exe") ||
        control_center::IsBlockingProcessName(L"not-wow.exe")) return 12;
    const auto before = control_center::CheckBlockingProcesses();
    if (before.state == control_center::ProcessState::Unknown) return 13;
    if (before.state == control_center::ProcessState::Stopped) {
        wchar_t module[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, module, MAX_PATH)) return 14;
        const std::wstring childPath = base + L"\\wowclassic.exe";
        if (!CopyFileW(module, childPath.c_str(), TRUE)) return 15;
        const std::wstring eventName = L"Local\\WOW335-test-" + std::to_wstring(GetCurrentProcessId());
        HANDLE signal = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!signal) return 16;
        std::wstring command = L"\"" + childPath + L"\" --child " + eventName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(childPath.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process)) { CloseHandle(signal); return 17; }
        bool found = false;
        for (int i = 0; i < 40; ++i) {
            const auto current = control_center::CheckBlockingProcesses();
            if (current.state == control_center::ProcessState::Running && _wcsicmp(current.name.c_str(), L"wowclassic.exe") == 0) { found = true; break; }
            Sleep(50);
        }
        SetEvent(signal);
        const auto finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(signal);
        if (!found || finished != WAIT_OBJECT_0) return 18;
    } else {
        std::puts("process child case skipped: a blocking process was already running");
    }
    std::puts("client: x86, x64, malformed, protected root, junction and process checks passed");
    return 0;
}
