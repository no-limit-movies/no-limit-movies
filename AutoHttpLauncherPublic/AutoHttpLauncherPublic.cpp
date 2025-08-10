#include <filesystem>
#include <iostream>
#include "httplib.h"
#include <windows.h>
#include <tlhelp32.h> // For process snapshot
#include <random>

using namespace std;
using namespace httplib;
namespace fs = filesystem;

bool isProcessRunning(const wstring& processName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        wcout << L"[ERROR] Failed to create snapshot\n";
        return false;
    }

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnap, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0) {
                wcout << L"[INFO] Process found running: " << processName << L"\n";
                CloseHandle(hSnap);
                return true;
            }
        } while (Process32NextW(hSnap, &pe32));
    }

    CloseHandle(hSnap);
    return false;
}


bool terminateProcesses(const vector<wstring>& processNames) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        wcout << L"[ERROR] Unable to create process snapshot\n";
        return false;
    }

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    bool anyTerminated = false;

    if (Process32FirstW(hSnap, &pe32)) {
        do {
            for (const auto& name : processNames) {
                if (_wcsicmp(pe32.szExeFile, name.c_str()) == 0) { // Case-insensitive match
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProcess) {
                        if (TerminateProcess(hProcess, 0)) {
                            wcout << L"[INFO] Terminated process: " << name << L" (PID: " << pe32.th32ProcessID << L")\n";
                            anyTerminated = true;
                        }
                        else {
                            wcout << L"[ERROR] Failed to terminate: " << name << L" (PID: " << pe32.th32ProcessID << L")\n";
                        }
                        CloseHandle(hProcess);
                    }
                    else {
                        wcout << L"[ERROR] Cannot open process: " << name << L" (PID: " << pe32.th32ProcessID << L")\n";
                    }
                }
            }
        } while (Process32NextW(hSnap, &pe32));
    }
    else {
        wcout << L"[ERROR] Process32FirstW failed\n";
    }

    CloseHandle(hSnap);
    return anyTerminated;
}


string escapeJson(const string& str) {
    string escaped;
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(str[i]);

        switch (c) {
        case '\"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[7];
                sprintf_s(buffer, "\\u%04x", c);
                escaped += buffer;
            }
            else {
                escaped += c; // do not split UTF-8 chars!
            }
        }
    }
    return escaped;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    Server svr;

    // Hide the console window on launch
    HWND hwnd = GetConsoleWindow();
    if (hwnd != NULL) {
        ShowWindow(hwnd, SW_HIDE);
    }

    svr.Get("/automate", [](const Request& req, Response& res) {
        wcout << L"[INFO] /download (toggle) HTTP request received\n";

        wstring exePath = L"YOUR_DOWNLOAD_FULL_AUTOMATION_FULL_PATH";
        wstring exeName = L"DownloadFullAutomation.exe";

        if (isProcessRunning(exeName)) {
            // Turn OFF: terminate the process
            if (terminateProcesses({ exeName })) {
                res.set_content("DownloadFullAutomation is OFF", "text/plain");
                wcout << L"[INFO] DownloadFullAutomation.exe terminated (turned OFF)\n";
            }
            else {
                res.set_content("Failed to terminate DownloadFullAutomation", "text/plain");
                wcout << L"[ERROR] Failed to terminate DownloadFullAutomation.exe\n";
            }
        }
        else {
            // Turn ON: launch the process in background
            STARTUPINFOW si = { 0 };
            PROCESS_INFORMATION pi = { 0 };
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;  // Hide window

            wchar_t* cmdLine = _wcsdup(exePath.c_str());

            BOOL success = CreateProcessW(
                NULL, cmdLine, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

            free(cmdLine);

            if (success) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                res.set_content("DownloadFullAutomation is ON", "text/plain");
                wcout << L"[INFO] DownloadFullAutomation.exe launched in background (turned ON)\n";
            }
            else {
                DWORD err = GetLastError();
                res.set_content("Failed to launch DownloadFullAutomation. Error: " + to_string(err), "text/plain");
                wcout << L"[ERROR] Failed to launch DownloadFullAutomation. Error: " << err << L"\n";
            }
        }
        });

    // Route: /launch
    svr.Get("/launch", [](const Request& req, Response& res) {
        wcout << L"[INFO] /launch HTTP request received\n";

        wstring exePath = L"YOUR_NO_LIMIT_MOVIES_FULL_PATH";
        wstring exeName = L"No Limit Movies - Public.exe";
        wstring mpvName = L"mpv.exe";

        if (isProcessRunning(exeName) || isProcessRunning(mpvName)) {
            bool terminated = terminateProcesses({ exeName, mpvName });
            if (terminated) {
                res.set_content("Processes terminated", "text/plain");
            }
            else {
                res.set_content("Failed to terminate processes", "text/plain");
            }
            return;
        }

        // Launch the program with autorun
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        wstring cmdLineFull = L"\"" + exePath + L"\" autorun";
        wchar_t* cmdLine = _wcsdup(cmdLineFull.c_str());

        BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        free(cmdLine);

        if (success) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            res.set_content("Launched with autorun", "text/plain");
            wcout << L"[INFO] Process launched with 'autorun' argument\n";
        }
        else {
            DWORD err = GetLastError();
            wcout << L"[ERROR] CreateProcessW failed with error code " << err << L"\n";
            res.set_content("Failed to launch", "text/plain");
        }
        });

    wcout << L"[INFO] Auto launcher listening on port 8060...\n";
    svr.listen("0.0.0.0", 8060);
    return 0;
}
