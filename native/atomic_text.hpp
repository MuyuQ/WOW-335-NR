#pragma once
#include <windows.h>
#include <atomic>
#include <string>

namespace nr_files {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// Keep writers out while reading and replacing. ReplaceFile retains the old bytes
// in a uniquely named backup; no failed read is interpreted as an empty file.
template<class Edit> bool EditText(const std::wstring &path, Edit edit) {
    Handle source;
    source.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    const bool exists=source.value!=INVALID_HANDLE_VALUE;
    if (!exists && GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
    std::string text;
    if (exists) {
        char buf[4096];
        DWORD read=0;
        for (;;) {
            if (!ReadFile(source.value,buf,sizeof buf,&read,nullptr)) return false;
            if (!read) break;
            text.append(buf,read);
        }
    }
    if (!edit(text)) return false;
    static std::atomic<unsigned long> serial{0};
    const std::wstring suffix=L".nr-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++serial);
    const std::wstring temp=path+suffix+L".tmp",backup=path+suffix+L".bak";
    bool ok=false;
    {
        Handle file;
        file.value=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file.value==INVALID_HANDLE_VALUE) return false;
        DWORD written=0;
        ok=text.size()<=MAXDWORD && WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) &&
           written==text.size() && FlushFileBuffers(file.value);
    }
    if (ok && exists) {
        // A path rename/replacement by another process must not redirect this edit.
        Handle current;
        current.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                                  nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        BY_HANDLE_FILE_INFORMATION a{},b{};
        ok=current.value!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(source.value,&a) &&
           GetFileInformationByHandle(current.value,&b) && a.dwVolumeSerialNumber==b.dwVolumeSerialNumber &&
           a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow;
        if (ok) ok=ReplaceFileW(path.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)!=FALSE;
    } else if (ok) {
        ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    }
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
}
