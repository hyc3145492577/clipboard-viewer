// 剪贴板查看器 - 原生 Win32 版（无 .NET 依赖，单文件可分发）
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <ctime>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <string>

using std::wstring;
using std::vector;

#define APP_CLASS  L"ClipViewerNativeWnd"
#define PIC_CLASS  L"ClipViewerPicPane"
#define ID_TIMER   1
#define WM_TRAYMSG (WM_APP + 1)
#define WM_SHOWME  (WM_APP + 2)
#define HOTKEY_ID  1
#define IDM_SHOW   1001
#define IDM_EXIT   1002

enum Kind { K_NONE = 0, K_TEXT = 1, K_IMAGE = 2, K_FILES = 3 };

static HINSTANCE g_inst;
static HWND g_hwnd, g_hEdit, g_hPic, g_hFiles, g_hHist, g_hTabs, g_hStatus;
static HWND g_btnRefresh, g_btnCopy, g_btnSave, g_btnClear, g_chkAuto, g_chkTop;
static HWND g_lblHist;
static HFONT g_fUi, g_fMono;
static HBRUSH g_brushBg;
static int  g_dpi = 96;
static bool g_auto = true;
static bool g_exit = false;
static bool g_hotkey = false;
static HICON g_icon = NULL;
static HBITMAP g_bmp = NULL;
static int g_bmpW = 0, g_bmpH = 0;
static Kind g_kind = K_NONE;
static wstring g_text, g_sig, g_statusMsg;
static vector<wstring> g_hist;
static vector<wstring> g_fileList;
static NOTIFYICONDATAW g_nid;

static int S(int v) { return v * g_dpi / 96; }

static wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, _countof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static wstring NowTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Fmt(L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

static unsigned long long HashW(const wstring& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.size(); i++) {
        h ^= (unsigned long long)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void SetStatus(const wstring& msg) {
    g_statusMsg = msg;
    wstring state = g_auto ? L"自动刷新中" : L"已暂停";
    if (g_hotkey) state += L" · Ctrl+Alt+V 唤出";
    SetWindowTextW(g_hStatus, (L"  " + state + L"  ·  " + msg).c_str());
}

// ---------------- 图标 ----------------
static HICON BuildIcon() {
    const int n = 32;
    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;

    HDC mem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mem, color);
    RECT r = { 0, 0, n, n };
    HBRUSH bg = CreateSolidBrush(RGB(33, 99, 211));
    FillRect(mem, &r, bg);
    DeleteObject(bg);
    HBRUSH page = CreateSolidBrush(RGB(255, 255, 255));
    RECT rp = { 8, 8, 24, 27 };
    FillRect(mem, &rp, page);
    DeleteObject(page);
    HBRUSH clip = CreateSolidBrush(RGB(255, 186, 0));
    RECT rc2 = { 11, 4, 21, 10 };
    FillRect(mem, &rc2, clip);
    DeleteObject(clip);
    HBRUSH line = CreateSolidBrush(RGB(150, 160, 175));
    RECT l1 = { 11, 13, 21, 15 }, l2 = { 11, 17, 21, 19 }, l3 = { 11, 21, 18, 23 };
    FillRect(mem, &l1, line);
    FillRect(mem, &l2, line);
    FillRect(mem, &l3, line);
    DeleteObject(line);
    SelectObject(mem, old);
    DeleteDC(mem);

    HBITMAP mask = CreateBitmap(n, n, 1, 1, NULL);
    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ico = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ico;
}

// ---------------- 图片页 ----------------
static LRESULT CALLBACK PicProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (g_bmp && g_bmpW > 0 && g_bmpH > 0) {
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            double sx = (double)cw / g_bmpW, sy = (double)ch / g_bmpH;
            double s = sx < sy ? sx : sy;
            if (s > 1.0) s = 1.0;
            int dw = (int)(g_bmpW * s), dh = (int)(g_bmpH * s);
            int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g_bmp);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, dx, dy, dw, dh, mem, 0, 0, g_bmpW, g_bmpH, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void DeleteImage() {
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = NULL; }
    g_bmpW = g_bmpH = 0;
    if (g_hPic) InvalidateRect(g_hPic, NULL, TRUE);
}

// ---------------- 页面切换 ----------------
static void UpdatePage() {
    int sel = TabCtrl_GetCurSel(g_hTabs);
    ShowWindow(g_hEdit, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hPic, sel == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hFiles, sel == 2 ? SW_SHOW : SW_HIDE);
}

static void SelectTab(int idx) {
    TabCtrl_SetCurSel(g_hTabs, idx);
    UpdatePage();
}

// ---------------- 布局 ----------------
static void LayoutChildren(int w, int h) {
    int pad = S(8), topH = S(46), statusH = S(26), histH = S(118);

    int x = pad, y = S(13), bh = S(24);
    MoveWindow(g_chkAuto, x, y, S(132), bh, TRUE); x += S(140);
    MoveWindow(g_chkTop, x, y, S(92), bh, TRUE); x += S(104);
    MoveWindow(g_btnRefresh, x, y - S(2), S(86), S(27), TRUE); x += S(92);
    MoveWindow(g_btnCopy, x, y - S(2), S(66), S(27), TRUE); x += S(72);
    MoveWindow(g_btnSave, x, y - S(2), S(78), S(27), TRUE); x += S(84);
    MoveWindow(g_btnClear, x, y - S(2), S(88), S(27), TRUE);

    int tabsY = topH;
    int tabsH = h - statusH - histH - topH - pad / 2;
    if (tabsH < S(120)) tabsH = S(120);
    MoveWindow(g_hTabs, pad, tabsY, w - 2 * pad, tabsH, TRUE);

    RECT rc = { 0, 0, w - 2 * pad, tabsH };
    TabCtrl_AdjustRect(g_hTabs, FALSE, &rc);
    int innerX = rc.left + pad, innerY = rc.top + tabsY;
    int innerW = rc.right - rc.left, innerH = rc.bottom - rc.top;
    if (innerW < S(40)) innerW = S(40);
    if (innerH < S(40)) innerH = S(40);
    MoveWindow(g_hEdit, innerX, innerY, innerW, innerH, TRUE);
    MoveWindow(g_hPic, innerX, innerY, innerW, innerH, TRUE);
    MoveWindow(g_hFiles, innerX, innerY, innerW, innerH, TRUE);

    int histY = tabsY + tabsH + pad / 2;
    MoveWindow(g_lblHist, pad + S(2), histY, w - 2 * pad, S(18), TRUE);
    MoveWindow(g_hHist, pad, histY + S(20), w - 2 * pad, histH - S(26), TRUE);

    MoveWindow(g_hStatus, 0, h - statusH, w, statusH, TRUE);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

// ---------------- 历史 ----------------
static void RefreshHistoryList() {
    SendMessageW(g_hHist, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_hist.size(); i++) {
        wstring one = g_hist[i];
        for (size_t k = 0; k < one.size(); k++) {
            if (one[k] == L'\r' || one[k] == L'\n') one[k] = L' ';
        }
        if (one.size() > 110) one = one.substr(0, 110) + L"…";
        wstring item = Fmt(L"%2d. ", (int)(i + 1)) + one;
        SendMessageW(g_hHist, LB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
}

static void AddHistory(const wstring& t) {
    if (t.empty()) return;
    for (size_t i = 0; i < g_hist.size(); i++) {
        if (g_hist[i] == t) { g_hist.erase(g_hist.begin() + i); break; }
    }
    g_hist.insert(g_hist.begin(), t);
    while (g_hist.size() > 30) g_hist.pop_back();
    RefreshHistoryList();
}

// ---------------- 显示 ----------------
static void ShowEmpty() {
    if (g_sig == L"E") return;
    g_sig = L"E";
    g_kind = K_NONE;
    g_text.clear();
    g_fileList.clear();
    SetWindowTextW(g_hEdit, L"");
    DeleteImage();
    SendMessageW(g_hFiles, LB_RESETCONTENT, 0, 0);
    SetStatus(L"剪贴板为空 · " + NowTime());
}

static void ShowText(const wstring& t) {
    if (t.empty()) { ShowEmpty(); return; }
    wstring sig = L"T" + Fmt(L"%u", (unsigned)t.size()) + Fmt(L"-%llx", HashW(t));
    if (sig == g_sig) return;
    g_sig = sig;
    g_kind = K_TEXT;
    g_text = t;
    g_fileList.clear();
    DeleteImage();

    wstring shown = t;
    if (shown.size() > 300000) {
        shown = shown.substr(0, 300000) + L"\r\n\r\n……（内容过长，仅显示前 300000 个字符）";
    }
    SetWindowTextW(g_hEdit, shown.c_str());
    SendMessageW(g_hEdit, EM_SETSEL, 0, 0);
    if (TabCtrl_GetCurSel(g_hTabs) != 0) SelectTab(0);
    AddHistory(t);
    SetStatus(Fmt(L"文本 · %u 字符 · ", (unsigned)t.size()) + NowTime());
}

static void ShowImage(HBITMAP bmp, int w, int h, const wstring& sig) {
    if (sig == g_sig) { if (bmp) DeleteObject(bmp); return; }
    g_sig = sig;
    g_kind = K_IMAGE;
    g_text.clear();
    g_fileList.clear();
    DeleteImage();
    g_bmp = bmp;
    g_bmpW = w;
    g_bmpH = h;
    if (TabCtrl_GetCurSel(g_hTabs) != 1) SelectTab(1);
    InvalidateRect(g_hPic, NULL, TRUE);
    SetStatus(Fmt(L"图片 · %d×%d 像素 · 可直接「保存…」", w, h));
}

static void ShowFiles(const vector<wstring>& paths) {
    wstring sig = L"F";
    for (size_t i = 0; i < paths.size(); i++) sig += paths[i] + L"|";
    if (sig == g_sig) return;
    g_sig = sig;
    g_kind = K_FILES;
    g_text.clear();
    g_fileList = paths;
    DeleteImage();
    SendMessageW(g_hFiles, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < paths.size(); i++)
        SendMessageW(g_hFiles, LB_ADDSTRING, 0, (LPARAM)paths[i].c_str());
    if (TabCtrl_GetCurSel(g_hTabs) != 2) SelectTab(2);
    SetStatus(Fmt(L"文件 · %u 项 · 双击可在资源管理器中打开", (unsigned)paths.size()));
}

// ---------------- 读剪贴板 ----------------
static void PollClipboard() {
    if (!g_auto) return;

    bool opened = false;
    for (int i = 0; i < 3 && !opened; i++) {
        if (OpenClipboard(g_hwnd)) opened = true;
        else Sleep(10);
    }
    if (!opened) return;

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            HDROP drop = (HDROP)h;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            vector<wstring> paths;
            for (UINT i = 0; i < n; i++) {
                wchar_t buf[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, buf, _countof(buf)) > 0) paths.push_back(buf);
            }
            CloseClipboard();
            if (!paths.empty()) { ShowFiles(paths); return; }
            ShowEmpty();
            return;
        }
    }

    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h) {
            BITMAPINFO* bi = (BITMAPINFO*)GlobalLock(h);
            if (bi) {
                int w = bi->bmiHeader.biWidth, hh = bi->bmiHeader.biHeight;
                int bc = bi->bmiHeader.biBitCount;
                size_t palSize = 0;
                if (bc <= 8) {
                    size_t colors = bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed : ((size_t)1 << bc);
                    palSize = colors * sizeof(RGBQUAD);
                }
                void* bits = (BYTE*)bi + sizeof(BITMAPINFOHEADER) + palSize;
                size_t stride = (((size_t)w * bc + 31) / 32) * 4;
                size_t imgSize = stride * (size_t)(hh < 0 ? -hh : hh);
                unsigned long long sample = 0;
                if (imgSize > 0) {
                    const unsigned char* p = (const unsigned char*)bits;
                    for (size_t k = 0; k < 128 && k < imgSize; k++) sample = sample * 31 + p[k];
                }
                wstring sig = Fmt(L"I%d-%d-%d-%llu-%llx", w, hh, bc, (unsigned long long)imgSize, sample);
                if (sig == g_sig) { GlobalUnlock(h); CloseClipboard(); return; }
                HDC dc = GetDC(NULL);
                HBITMAP bmp = CreateDIBitmap(dc, &bi->bmiHeader, CBM_INIT, bits, bi, DIB_RGB_COLORS);
                ReleaseDC(NULL, dc);
                GlobalUnlock(h);
                CloseClipboard();
                if (bmp) { ShowImage(bmp, w, hh < 0 ? -hh : hh, sig); return; }
                ShowEmpty();
                return;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t* p = (wchar_t*)GlobalLock(h);
            if (p) {
                wstring t(p);
                GlobalUnlock(h);
                CloseClipboard();
                ShowText(t);
                return;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_TEXT)) {
        HANDLE h = GetClipboardData(CF_TEXT);
        if (h) {
            char* p = (char*)GlobalLock(h);
            if (p) {
                int n = MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
                wstring t(n > 0 ? n - 1 : 0, L'\0');
                if (n > 0) MultiByteToWideChar(CP_ACP, 0, p, -1, &t[0], n);
                GlobalUnlock(h);
                CloseClipboard();
                ShowText(t);
                return;
            }
        }
    }

    CloseClipboard();
    ShowEmpty();
}

// ---------------- 复制 / 保存 ----------------
static bool SetClipText(const wstring& t) {
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) { CloseClipboard(); return false; }
    void* p = GlobalLock(g);
    memcpy(p, t.c_str(), bytes);
    GlobalUnlock(g);
    bool ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
    if (!ok) GlobalFree(g);
    CloseClipboard();
    return ok;
}

static void CopyCurrent() {
    if (g_kind == K_TEXT && !g_text.empty()) {
        if (SetClipText(g_text)) SetStatus(L"文本已复制到剪贴板");
        else SetStatus(L"复制失败：剪贴板被占用");
        return;
    }
    if (g_kind == K_IMAGE && g_bmp) {
        if (!OpenClipboard(g_hwnd)) { SetStatus(L"复制失败：剪贴板被占用"); return; }
        EmptyClipboard();
        HBITMAP copy = (HBITMAP)CopyImage(g_bmp, IMAGE_BITMAP, 0, 0, 0);
        if (copy) SetClipboardData(CF_BITMAP, copy);
        CloseClipboard();
        SetStatus(copy ? L"图片已复制到剪贴板" : L"复制失败");
        return;
    }
    if (g_kind == K_FILES && !g_fileList.empty()) {
        size_t total = sizeof(DROPFILES);
        for (size_t i = 0; i < g_fileList.size(); i++) total += (g_fileList[i].size() + 1) * sizeof(wchar_t);
        total += sizeof(wchar_t);
        if (!OpenClipboard(g_hwnd)) { SetStatus(L"复制失败：剪贴板被占用"); return; }
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
        if (g) {
            DROPFILES* df = (DROPFILES*)GlobalLock(g);
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            wchar_t* p = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
            for (size_t i = 0; i < g_fileList.size(); i++) {
                memcpy(p, g_fileList[i].c_str(), (g_fileList[i].size() + 1) * sizeof(wchar_t));
                p += g_fileList[i].size() + 1;
            }
            *p = 0;
            GlobalUnlock(g);
            if (!SetClipboardData(CF_HDROP, g)) GlobalFree(g);
        }
        CloseClipboard();
        SetStatus(L"文件列表已复制到剪贴板");
        return;
    }
    SetStatus(L"当前没有可复制的内容");
}

static bool SaveBmp(const wstring& path) {
    if (!g_bmp) return false;
    BITMAP bm;
    if (!GetObjectW(g_bmp, sizeof(bm), &bm)) return false;
    int w = bm.bmWidth, h = bm.bmHeight;
    size_t stride = (((size_t)w * 24 + 31) / 32) * 4;
    size_t imgSize = stride * (size_t)h;
    BITMAPINFOHEADER bih;
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)imgSize;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    BITMAPFILEHEADER fh;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bih);
    fh.bfSize = (DWORD)(fh.bfOffBits + imgSize);

    std::vector<BYTE> pixels(imgSize);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader = bih;
    HDC dc = GetDC(NULL);
    int lines = GetDIBits(dc, g_bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);

    DWORD written = 0;
    bool ok = lines > 0;
    if (ok) {
        WriteFile(f, &fh, sizeof(fh), &written, NULL);
        WriteFile(f, &bih, sizeof(bih), &written, NULL);
        WriteFile(f, pixels.data(), (DWORD)imgSize, &written, NULL);
    }
    CloseHandle(f);
    return ok;
}

static void SaveCurrent() {
    if (g_kind == K_NONE) { SetStatus(L"当前没有可保存的内容"); return; }

    wchar_t path[MAX_PATH] = L"";
    wchar_t name[128];
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_kind == K_IMAGE)
        swprintf(name, 128, L"剪贴板图片_%04d%02d%02d_%02d%02d%02d.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else
        swprintf(name, 128, L"剪贴板文本_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wcsncpy(path, name, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = (g_kind == K_IMAGE) ? L"BMP 图片\0*.bmp\0所有文件\0*.*\0" : L"文本文件\0*.txt\0所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    bool ok = false;
    if (g_kind == K_IMAGE) {
        ok = SaveBmp(path);
    } else if (g_kind == K_FILES) {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            wstring all;
            for (size_t i = 0; i < g_fileList.size(); i++) all += g_fileList[i] + L"\r\n";
            int len = WideCharToMultiByte(CP_UTF8, 0, all.c_str(), -1, NULL, 0, NULL, NULL);
            std::string utf8(len > 0 ? len - 1 : 0, '\0');
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, all.c_str(), -1, &utf8[0], len, NULL, NULL);
            DWORD written = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, NULL);
            CloseHandle(f);
            ok = true;
        }
    } else {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            int len = WideCharToMultiByte(CP_UTF8, 0, g_text.c_str(), -1, NULL, 0, NULL, NULL);
            std::string utf8(len > 0 ? len - 1 : 0, '\0');
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, g_text.c_str(), -1, &utf8[0], len, NULL, NULL);
            DWORD written = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, NULL);
            CloseHandle(f);
            ok = true;
        }
    }
    SetStatus(ok ? (L"已保存：" + wstring(path)) : L"保存失败");
}

// ---------------- 托盘 ----------------
static void ShowMe() {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    g_sig.clear();
    PollClipboard();
}

static void ExitApp() {
    g_exit = true;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
}

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"打开窗口   (Ctrl+Alt+V)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == IDM_SHOW) ShowMe();
    else if (cmd == IDM_EXIT) ExitApp();
}

static void SetupTray() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"剪贴板查看器 · Ctrl+Alt+V 打开", _countof(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

// ---------------- 窗口过程 ----------------
static void UpdateAutoStatus() {
    SetStatus(g_statusMsg);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        LayoutChildren(rc.right - rc.left, rc.bottom - rc.top);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = S(560);
        mmi->ptMinTrackSize.y = S(420);
        return 0;
    }
    case WM_TIMER:
        if (w == ID_TIMER) PollClipboard();
        return 0;
    case WM_HOTKEY:
        if (w == HOTKEY_ID) ShowMe();
        return 0;
    case WM_SHOWME:
        ShowMe();
        return 0;
    case WM_TRAYMSG: {
        UINT ev = (UINT)LOWORD(l);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) { ShowTrayMenu(); return 0; }
        if (ev == WM_LBUTTONDBLCLK || ev == NIN_SELECT || ev == NIN_KEYSELECT) { ShowMe(); return 0; }
        if (ev == WM_LBUTTONUP) { ShowMe(); return 0; }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == 100 && code == BN_CLICKED) {           // 自动刷新
            g_auto = SendMessageW(g_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_auto) { SetStatus(L"自动刷新已开启"); PollClipboard(); }
            else SetStatus(L"自动刷新已暂停");
            return 0;
        }
        if (id == 101 && code == BN_CLICKED) {           // 置顶
            bool on = SendMessageW(g_chkTop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            return 0;
        }
        if (id == 102 && code == BN_CLICKED) { g_sig.clear(); PollClipboard(); SetStatus(L"已手动刷新"); return 0; }
        if (id == 103 && code == BN_CLICKED) { CopyCurrent(); return 0; }
        if (id == 104 && code == BN_CLICKED) { SaveCurrent(); return 0; }
        if (id == 105 && code == BN_CLICKED) {
            g_hist.clear();
            RefreshHistoryList();
            SetStatus(L"历史已清空");
            return 0;
        }
        if (id == 200 && code == LBN_DBLCLK) {           // 历史双击
            int idx = (int)SendMessageW(g_hHist, LB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < (int)g_hist.size()) {
                if (SetClipText(g_hist[idx])) SetStatus(Fmt(L"已把第 %d 条历史复制到剪贴板", idx + 1));
                else SetStatus(L"复制失败：剪贴板被占用");
            }
            return 0;
        }
        if (id == 201 && code == LBN_DBLCLK) {           // 文件双击
            int idx = (int)SendMessageW(g_hFiles, LB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < (int)g_fileList.size())
                ShellExecuteW(NULL, L"open", g_fileList[idx].c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)l;
        if (nh->idFrom == 300 && nh->code == TCN_SELCHANGE) { UpdatePage(); return 0; }
        return 0;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)g_brushBg;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, RGB(60, 60, 60));
        return (LRESULT)g_brushBg;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_brushBg);
        return 1;
    }
    case WM_CLOSE:
        if (!g_exit) {
            ShowWindow(h, SW_HIDE);
            SetStatus(L"已收进托盘 · 按 Ctrl+Alt+V 打开");
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER);
        if (g_hotkey) UnregisterHotKey(h, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 初始化 ----------------
static HFONT MakeFont(const wchar_t* face, int pt, bool bold) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void CreateControls(HWND h) {
    HINSTANCE hi = g_inst;
    g_brushBg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

    g_chkAuto = CreateWindowExW(0, L"BUTTON", L"自动刷新 (0.5 秒)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, h, (HMENU)100, hi, NULL);
    SendMessageW(g_chkAuto, BM_SETCHECK, BST_CHECKED, 0);

    g_chkTop = CreateWindowExW(0, L"BUTTON", L"窗口置顶",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, h, (HMENU)101, hi, NULL);

    g_btnRefresh = CreateWindowExW(0, L"BUTTON", L"立即刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)102, hi, NULL);
    g_btnCopy = CreateWindowExW(0, L"BUTTON", L"复制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)103, hi, NULL);
    g_btnSave = CreateWindowExW(0, L"BUTTON", L"保存…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)104, hi, NULL);
    g_btnClear = CreateWindowExW(0, L"BUTTON", L"清空历史", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)105, hi, NULL);

    g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, h, (HMENU)300, hi, NULL);
    TCITEMW ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"文本";
    TabCtrl_InsertItem(g_hTabs, 0, &ti);
    ti.pszText = (LPWSTR)L"图片";
    TabCtrl_InsertItem(g_hTabs, 1, &ti);
    ti.pszText = (LPWSTR)L"文件";
    TabCtrl_InsertItem(g_hTabs, 2, &ti);

    g_hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, h, NULL, hi, NULL);
    SendMessageW(g_hEdit, EM_SETLIMITTEXT, 1000000, 0);

    g_hPic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD, 0, 0, 0, 0, h, NULL, hi, NULL);

    g_hFiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 0, 0, 0, 0, h, (HMENU)201, hi, NULL);

    g_lblHist = CreateWindowExW(0, L"STATIC", L"文本历史（双击复制回剪贴板）", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, NULL, hi, NULL);
    g_hHist = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 0, 0, 0, 0, h, (HMENU)200, hi, NULL);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, h, NULL, hi, NULL);

    HFONT f = MakeFont(L"Microsoft YaHei UI", 9, false);
    g_fUi = f;
    g_fMono = MakeFont(L"Consolas", 10, false);

    HWND all[] = { g_chkAuto, g_chkTop, g_btnRefresh, g_btnCopy, g_btnSave, g_btnClear,
                   g_hTabs, g_hFiles, g_hHist, g_lblHist, g_hStatus };
    for (int i = 0; i < _countof(all); i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_fUi, TRUE);
    SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_fMono, TRUE);

    // 给列表/编辑框加一点内边距效果：字体统一后再设一次
    SendMessageW(g_hPic, WM_SETFONT, (WPARAM)g_fUi, TRUE);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    g_inst = inst;

    HANDLE mutex = CreateMutexW(NULL, FALSE, L"ClipboardViewerNative.SingleInstance.7C1D");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        if (other) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }

    SetProcessDPIAware();
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = g_icon = BuildIcon();
    wc.hIconSm = g_icon;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PicProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    pc.lpszClassName = PIC_CLASS;
    RegisterClassExW(&pc);

    int w = S(820), hgt = S(660);
    g_hwnd = CreateWindowExW(0, APP_CLASS, L"剪贴板查看器",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, hgt, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 0;

    CreateControls(g_hwnd);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    LayoutChildren(rc.right - rc.left, rc.bottom - rc.top);
    UpdatePage();

    g_hotkey = RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V') != FALSE;

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    SetupTray();
    SetTimer(g_hwnd, ID_TIMER, 500, NULL);

    SetStatus(L"正在读取剪贴板…");
    PollClipboard();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
