#include <windows.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <set>

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TRAY_TOGGLE  1001
#define ID_TRAY_EXIT    1002
#define IDI_MY_ICON     101
#define IDT_TIMER       2001

// 감시 대상 프로세스 목록
const std::vector<std::wstring> TARGET_PROCESSES = {
    L"notepad.exe",
    L"WindowsNotepad.exe",
    L"Microsoft.Notes.exe",
    L"MicrosoftStickyNotes.exe",
    L"StickyNotes.exe"
};

// 전역 변수
NOTIFYICONDATA g_nid = { 0 };
HICON g_hIconColor = NULL;
HICON g_hIconGrayscale = NULL;
bool g_bEnabled = true;

// 실행 중인 대상 PID 수집
std::set<DWORD> GetTargetProcessIds()
{
    std::set<DWORD> pids;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            for (const auto& procName : TARGET_PROCESSES) {
                if (_wcsicmp(pe32.szExeFile, procName.c_str()) == 0) {
                    pids.insert(pe32.th32ProcessID);
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return pids;
}

struct EnumData {
    std::set<DWORD> processIds;
    bool setTopmost;
};

// UWP 자식 윈도우 순회용 콜백
struct ChildEnumData {
    const std::set<DWORD>* pPids;
    bool bFound;
};

BOOL CALLBACK EnumChildProc(HWND hChild, LPARAM lParam)
{
    ChildEnumData* pChildData = reinterpret_cast<ChildEnumData*>(lParam);
    DWORD dwChildPid = 0;
    GetWindowThreadProcessId(hChild, &dwChildPid);

    if (pChildData->pPids->find(dwChildPid) != pChildData->pPids->end()) {
        pChildData->bFound = true;
        return FALSE;
    }
    return TRUE;
}

// 윈도우가 이미 TOPMOST 상태인지 확인하는 함수
bool IsAlreadyTopmost(HWND hWnd)
{
    LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    return (exStyle & WS_EX_TOPMOST) != 0;
}

// 시스템 컨텍스트 메뉴(우클릭 팝업)가 현재 열려있는지 확인
bool IsContextMenuOpen()
{
    // #32768 은 Windows 시스템의 컨텍스트 메뉴 클래스 이름입니다.
    HWND hMenuWnd = FindWindow(_T("#32768"), NULL);
    return (hMenuWnd != NULL && IsWindowVisible(hMenuWnd));
}

// 최상위 윈도우 순회 콜백 (수정된 버전)
BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    EnumData* pData = reinterpret_cast<EnumData*>(lParam);

    // 최소화되어 있거나 창이 보이지 않는 경우 스킵
    if (!IsWindowVisible(hWnd) || IsIconic(hWnd))
        return TRUE;

    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hWnd, &dwProcessId);

    TCHAR szClassName[256] = { 0 };
    GetClassName(hWnd, szClassName, 256);

    bool bIsTarget = false;

    // 1. 일반 프로세스 PID 매칭
    if (pData->processIds.find(dwProcessId) != pData->processIds.end()) {
        bIsTarget = true;
    }

    // 2. UWP App Frame 검증 (기존 열려있던 창 잡기 핵심)
    if (!bIsTarget && (_tcscmp(szClassName, _T("ApplicationFrameWindow")) == 0 ||
        _tcscmp(szClassName, _T("Windows.UI.Core.CoreWindow")) == 0))
    {
        ChildEnumData childData = { &pData->processIds, false };
        EnumChildWindows(hWnd, EnumChildProc, reinterpret_cast<LPARAM>(&childData));
        if (childData.bFound) {
            bIsTarget = true;
        }
    }

    // 3. 최상단 속성 적용
    if (bIsTarget) {
        LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);

        // 툴팁이나 트레이 가상 창 등 제외
        if (!(exStyle & WS_EX_TOOLWINDOW)) {
            bool currentTopmost = IsAlreadyTopmost(hWnd);

            if (pData->setTopmost) {
                // 기존 창이든 새 창이든, TOPMOST가 아닌 상태라면 즉시 최상단 적용
                if (!currentTopmost) {
                    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                }
            }
            else {
                // 해제 요청 시 TOPMOST 상태면 해제
                if (currentTopmost) {
                    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                }
            }
        }
    }

    return TRUE;
}

void ApplyTopmostState(bool setTopmost)
{
    // 우클릭 컨텍스트 메뉴가 열려있는 동안에는 레이어 변경 스킵
    if (setTopmost && IsContextMenuOpen()) {
        return;
    }

    EnumData data;
    data.processIds = GetTargetProcessIds();
    data.setTopmost = setTopmost;

    // 현재 열린 모든 윈도우 스캔 및 조건 적용
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
}

// 아이콘 흑백 변환
HICON CreateGrayscaleIcon(HICON hIcon)
{
    ICONINFO iconInfo;
    if (!GetIconInfo(hIcon, &iconInfo)) return NULL;

    BITMAP bm;
    GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bm);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(NULL);
    DWORD* pPixels = new DWORD[bm.bmWidth * bm.bmHeight];

    GetDIBits(hdc, iconInfo.hbmColor, 0, bm.bmHeight, pPixels, &bmi, DIB_RGB_COLORS);

    for (int i = 0; i < bm.bmWidth * bm.bmHeight; ++i) {
        DWORD pixel = pPixels[i];
        BYTE a = (pixel >> 24) & 0xFF;
        BYTE r = (pixel >> 16) & 0xFF;
        BYTE g = (pixel >> 8) & 0xFF;
        BYTE b = pixel & 0xFF;

        BYTE gray = (BYTE)(0.299f * r + 0.587f * g + 0.114f * b);
        pPixels[i] = (a << 24) | (gray << 16) | (gray << 8) | gray;
    }

    SetDIBits(hdc, iconInfo.hbmColor, 0, bm.bmHeight, pPixels, &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    delete[] pPixels;

    HICON hGrayIcon = CreateIconIndirect(&iconInfo);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);

    return hGrayIcon;
}

// 트레이 우클릭 메뉴
void ShowContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        UINT uFlags = MF_STRING | (g_bEnabled ? MF_CHECKED : MF_UNCHECKED);
        AppendMenu(hMenu, uFlags, ID_TRAY_TOGGLE, g_bEnabled ? _T("최상단 고정 비활성화") : _T("최상단 고정 활성화"));
        AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("종료"));

        SetForegroundWindow(hWnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);
    }
}

// 메시지 프로시저
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_hIconColor = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MY_ICON));
        if (!g_hIconColor) {
            g_hIconColor = LoadIcon(NULL, IDI_APPLICATION);
        }

        g_hIconGrayscale = CreateGrayscaleIcon(g_hIconColor);

        g_nid.cbSize = sizeof(NOTIFYICONDATA);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hIconColor;
        _tcscpy_s(g_nid.szTip, _T("StickyOnTop (Running)"));
        Shell_NotifyIcon(NIM_ADD, &g_nid);

        // 타이머 설정
        SetTimer(hWnd, IDT_TIMER, 500, NULL);
        ApplyTopmostState(true);
        break;

    case WM_TIMER:
        if (wParam == IDT_TIMER && g_bEnabled) {
            ApplyTopmostState(true);
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowContextMenu(hWnd);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_TOGGLE:
            g_bEnabled = !g_bEnabled;
            if (g_bEnabled) {
                g_nid.hIcon = g_hIconColor;
                _tcscpy_s(g_nid.szTip, _T("StickyOnTop (Running)"));
                ApplyTopmostState(true);
            }
            else {
                g_nid.hIcon = g_hIconGrayscale;
                _tcscpy_s(g_nid.szTip, _T("StickyOnTop (Paused)"));
                ApplyTopmostState(false);
            }
            Shell_NotifyIcon(NIM_MODIFY, &g_nid);
            break;

        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_TIMER);
        ApplyTopmostState(false);
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        if (g_hIconGrayscale) DestroyIcon(g_hIconGrayscale);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    const TCHAR szClassName[] = _T("StickyOnTopTrayClass");

    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = szClassName;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MY_ICON));

    if (!RegisterClassEx(&wc)) return 0;

    HWND hWnd = CreateWindowEx(0, szClassName, _T("StickyOnTop"), 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}