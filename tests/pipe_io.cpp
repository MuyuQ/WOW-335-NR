#define NOMINMAX
#include "../carrier/x86-host/src/pipe_io.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Pair {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    Pair() {
        static unsigned number = 0;
        const auto name = L"\\\\.\\pipe\\WOW335NR-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++number);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE, "create server");
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        Check(client != INVALID_HANDLE_VALUE, "open client");
        Check(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED, "connect");
    }
    ~Pair() { if (client != INVALID_HANDLE_VALUE) CloseHandle(client); if (server != INVALID_HANDLE_VALUE) CloseHandle(server); }
};
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--peer") { Sleep(80); return 0; }
    try {
        {
            Pair pair;
            std::thread sender([&] { DWORD n = 0; WriteFile(pair.server, "ab", 2, &n, nullptr); Sleep(15); WriteFile(pair.server, "cd", 2, &n, nullptr); });
            char message[4]{};
            const bool ok = feed_pipe::Read(pair.client, nullptr, message, sizeof(message), 1000);
            sender.join();
            Check(ok && std::string(message, 4) == "abcd", "partial reads must form a complete message");
        }
        {
            Pair pair; char message[4]{};
            const ULONGLONG start = GetTickCount64();
            Check(!feed_pipe::Read(pair.client, nullptr, message, sizeof(message), 50) && GetLastError() == ERROR_TIMEOUT, "silent peer must time out");
            Check(GetTickCount64() - start < 1500, "read exceeded its bounded deadline");
            DWORD n = 0; Check(WriteFile(pair.server, "next", 4, &n, nullptr) != FALSE, "write after cancel");
            Check(feed_pipe::Read(pair.client, nullptr, message, 4, 1000) && std::string(message, 4) == "next", "cancelled read must not consume the next message");
        }
        {
            Pair pair; std::vector<char> data(1024 * 1024, 'x');
            const ULONGLONG start = GetTickCount64();
            Check(!feed_pipe::Write(pair.client, nullptr, data.data(), static_cast<DWORD>(data.size()), 50) && GetLastError() == ERROR_TIMEOUT, "full pipe must time out");
            Check(GetTickCount64() - start < 1500, "write exceeded its bounded deadline");
        }
        {
            Pair pair;
            wchar_t exe[32768]{}; Check(GetModuleFileNameW(nullptr, exe, 32768) > 0, "test image path");
            std::wstring command = L"\"" + std::wstring(exe) + L"\" --peer";
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "start peer");
            char message[4]{};
            const ULONGLONG start = GetTickCount64();
            const bool ok = feed_pipe::Read(pair.client, process.hProcess, message, sizeof(message), 5000);
            const DWORD error = GetLastError();
            WaitForSingleObject(process.hProcess, 1000); CloseHandle(process.hProcess); CloseHandle(process.hThread);
            Check(!ok && error == ERROR_BROKEN_PIPE && GetTickCount64() - start < 2000, "peer exit must wake pending I/O");
        }
        {
            Pair pair; CloseHandle(pair.server); pair.server = INVALID_HANDLE_VALUE; char byte{};
            Check(!feed_pipe::Read(pair.client, nullptr, &byte, 1, 1000), "broken pipe must fail");
        }
        std::puts("PASS pipe partial reads, read/write deadlines, cancellation retirement, peer exit and broken pipe");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s (Win32 %lu)\n", error.what(), GetLastError()); return 1; }
}
