#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include "nes.h"
#include "hd.h"
#include "video.h"
#include "gpu.h"

namespace {
struct Resolution { int w, h; const wchar_t *label; };
const Resolution resolutions[] = {
    {768, 720, L"768 x 720  —  Original"},
    {1280, 720, L"1280 x 720  —  HD"},
    {1920, 1080, L"1920 x 1080  —  Full HD"},
    {2560, 1440, L"2560 x 1440  —  QHD"},
    {3840, 2160, L"3840 x 2160  —  4K UHD"},
};
constexpr int count = sizeof(resolutions) / sizeof(resolutions[0]);
int active = 2, pending = 2, selected = 0;
bool widescreen = true, pending_widescreen = true;
int overscan = 8, pending_overscan = 8;
constexpr int row_count = 8;
bool pending_truecolor = true;
wchar_t notification[256] = {};
ULONGLONG notification_until = 0;
bool notification_error = false;
bool fullscreen = false, pending_fullscreen = false, menu = false;
WINDOWPLACEMENT placement = {sizeof(WINDOWPLACEMENT)};
wchar_t settings[MAX_PATH] = {};
HDC output_dc = nullptr;
HBITMAP output_bitmap = nullptr;
HGDIOBJ previous_bitmap = nullptr;
HDC present_dc = nullptr;
HBITMAP present_bitmap = nullptr;
HGDIOBJ old_present_bitmap = nullptr;
int present_w = 0, present_h = 0;
RECT rows[row_count] = {};
RECT panel = {};
double ui_scale = 1;

int px(int n) { return (int)(n * ui_scale); }

void save() {
    wchar_t value[24];
    swprintf_s(value, L"%d", active);
    WritePrivateProfileStringW(L"Video", L"Resolution", value, settings);
    WritePrivateProfileStringW(L"Video", L"Fullscreen", fullscreen ? L"1" : L"0", settings);
    WritePrivateProfileStringW(L"Video", L"Widescreen", widescreen ? L"1" : L"0", settings);
    WritePrivateProfileStringW(L"Video", L"TrueColor", hd_truecolor() ? L"1" : L"0", settings);
    swprintf_s(value, L"%d", overscan);
    WritePrivateProfileStringW(L"Video", L"Overscan", value, settings);
}

void free_surface() {
    if (output_dc) {
        SelectObject(output_dc, previous_bitmap);
        DeleteObject(output_bitmap);
        DeleteDC(output_dc);
    }
    output_dc = nullptr;
    output_bitmap = nullptr;
}

bool surface(HWND hwnd, int resolution) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = resolutions[resolution].w;
    bi.bmiHeader.biHeight = -resolutions[resolution].h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(hwnd);
    HDC next_dc = CreateCompatibleDC(dc);
    void *bits = nullptr;
    HBITMAP next_bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(hwnd, dc);
    if (!next_dc || !next_bitmap) {
        if (next_dc) DeleteDC(next_dc);
        if (next_bitmap) DeleteObject(next_bitmap);
        return false;
    }
    free_surface();
    output_dc = next_dc;
    output_bitmap = next_bitmap;
    previous_bitmap = SelectObject(output_dc, output_bitmap);
    return true;
}

void position_window(HWND hwnd, bool next_fullscreen) {
    MONITORINFO monitor = {sizeof(MONITORINFO)};
    GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    if (next_fullscreen) {
        if (!fullscreen) GetWindowPlacement(hwnd, &placement);
        SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left,
            monitor.rcMonitor.bottom-monitor.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (fullscreen) SetWindowPlacement(hwnd, &placement);
        // Keep the entire window reachable even when selecting 4K on a smaller monitor.
        RECT rc = {0, 0, resolutions[active].w, resolutions[active].h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        int maxw = monitor.rcWork.right-monitor.rcWork.left;
        int maxh = monitor.rcWork.bottom-monitor.rcWork.top;
        int w = rc.right-rc.left, h = rc.bottom-rc.top;
        if (w > maxw) w = maxw;
        if (h > maxh) h = maxh;
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, monitor.rcWork.left+(maxw-w)/2,
            monitor.rcWork.top+(maxh-h)/2, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = next_fullscreen;
}

void open_menu(bool open) {
    menu = open;
    controller_state = 0;
    pending = active;
    pending_fullscreen = fullscreen;
    pending_widescreen = widescreen;
    pending_overscan = overscan;
    pending_truecolor = hd_truecolor();
    selected = 0;
}

void change(int direction) {
    if (selected == 0) pending = (pending + direction + count) % count;
    if (selected == 1) pending_fullscreen = !pending_fullscreen;
    if (selected == 2) pending_widescreen = !pending_widescreen;
    if (selected == 3) pending_overscan = pending_overscan ? 0 : 8;
    if (selected == 4) pending_truecolor = !pending_truecolor;
}

void activate(HWND hwnd) {
    if (selected < 5) { change(1); return; }
    if (selected == 5) {
        if (!surface(hwnd, pending)) {
            MessageBoxW(hwnd, L"It was not possible to create the image at this resolution.", L"Video", MB_OK | MB_ICONERROR);
            return;
        }
        active = pending;
        widescreen = pending_widescreen;
        overscan = pending_overscan;
        hd_set_truecolor(pending_truecolor);
        position_window(hwnd, pending_fullscreen);
        save();
    }
    if (selected == 6) open_menu(false);
    if (selected == 7) PostMessage(hwnd, WM_CLOSE, 0, 0);
}

void layout(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    ui_scale = client.right / 680.0;
    if (ui_scale > client.bottom / 710.0) ui_scale = client.bottom / 710.0;
    if (ui_scale > 1.6) ui_scale = 1.6;
    int w = px(620), h = px(650);
    panel = {(client.right-w)/2, (client.bottom-h)/2,
             (client.right+w)/2, (client.bottom+h)/2};
    for (int i = 0; i < row_count; ++i)
        rows[i] = {panel.left+px(28), panel.top+px(116+i*50),
                   panel.right-px(28), panel.top+px(162+i*50)};
}

void fill(HDC dc, const RECT &r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void text(HDC dc, const wchar_t *value, RECT rect, COLORREF color, int size,
          bool bold = false, UINT alignment = DT_LEFT) {
    HFONT font = CreateFontW(-px(size), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &rect, alignment | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(font);
}

void draw_menu(HWND hwnd, HDC dc) {
    layout(hwnd);
    const COLORREF gold = RGB(235, 197, 111), white = RGB(237, 239, 248);
    fill(dc, panel, RGB(16, 23, 39));
    HBRUSH outline = CreateSolidBrush(gold);
    FrameRect(dc, &panel, outline);
    DeleteObject(outline);
    RECT title = {panel.left+px(28), panel.top+px(20), panel.right-px(28), panel.top+px(58)};
    text(dc, L"VIDEO SETTINGS", title, gold, 24, true);
    RECT info = {title.left, panel.top+px(61), title.right, panel.top+px(94)};
    wchar_t status[160];
    swprintf_s(status, L"Saída: %d x %d  |  %s", resolutions[active].w,
        resolutions[active].h, fullscreen ? L"Full Screen" : L"Window");
    text(dc, status, info, RGB(164, 179, 202), 15);
    const wchar_t *labels[] = {L"Resolution", L"Mode", L"Format", L"Border", L"Colors", L"Apply Modifications", L"Back to Game", L"Exit Game"};
    for (int i = 0; i < row_count; ++i) {
        fill(dc, rows[i], i == selected ? RGB(52, 65, 86) : RGB(27, 36, 54));
        RECT r = rows[i];
        r.left += px(14); r.right -= px(14);
        if (i < 5) {
            RECT label = r; label.right = label.left + px(105);
            text(dc, labels[i], label, white, 16);
            r.left += px(105);
            wchar_t value[160];
            const wchar_t *choice = i == 0 ? resolutions[pending].label
                : i == 1 ? (pending_fullscreen ? L"Full screen without borders" : L"Window")
                : i == 2 ? (pending_widescreen ? L"16:9 filled (stretched)" : L"Original proportion")
                : i == 3 ? (pending_overscan ? L"Hide 8 rows above/below" : L"Show all 240 rows")
                : (pending_truecolor ? L"32-bit RGB · full textures" : L"Classic NES palette");
            swprintf_s(value, L"‹  %s  ›", choice);
            text(dc, value, r, i == selected ? gold : white, 16, true, DT_RIGHT);
        } else text(dc, labels[i], r, i == selected ? gold : white, 17, true);
    }
    RECT note = {title.left, panel.top+px(528), title.right, panel.top+px(554)};
    text(dc, L"Save states: F5 save · F9 load · F6 slot · F8 last session", note,
        RGB(164, 179, 202), 13);
    note.top = panel.top+px(560); note.bottom = panel.top+px(586);
    text(dc, L"↑ ↓ select   ← → change   Enter confirm   Esc / F2 return", note,
        RGB(164, 179, 202), 14);
    note.top = panel.top+px(598); note.bottom = panel.top+px(626);
    text(dc, L"Game paused  •  Settings saved upon applying", note, gold, 13);
}
} // namespace

void video_init(HWND hwnd) {
    GetModuleFileNameW(nullptr, settings, MAX_PATH);
    wchar_t *slash = wcsrchr(settings, L'\\');
    if (slash) wcscpy_s(slash+1, MAX_PATH-(slash+1-settings), L"ff1-video.ini");
    active = (int)GetPrivateProfileIntW(L"Video", L"Resolution", 2, settings);
    if (active < 0 || active >= count) active = 2;
    hd_set_truecolor(GetPrivateProfileIntW(L"Video",L"TrueColor",1,settings)!=0);
    widescreen = GetPrivateProfileIntW(L"Video", L"Widescreen", 1, settings) != 0;
    overscan = GetPrivateProfileIntW(L"Video", L"Overscan", 8, settings) == 0 ? 0 : 8;
    bool start_fullscreen = GetPrivateProfileIntW(L"Video", L"Fullscreen", 0, settings) == 1;
    if (!surface(hwnd, active)) { active = 0; surface(hwnd, active); }
    position_window(hwnd, start_fullscreen);
}

bool video_menu_open() { return menu; }

bool video_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_GETMINMAXINFO) {
        MINMAXINFO *limits = reinterpret_cast<MINMAXINFO *>(lp);
        limits->ptMinTrackSize = {640, 480};
        return true;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        bool repeat = (lp & (1LL << 30)) != 0;
        if (wp == VK_F2 || wp == VK_ESCAPE) {
            if (!repeat) open_menu(!menu);
            return true;
        }
        if (wp == VK_F11) {
            if (!repeat) {
                position_window(hwnd, !fullscreen);
                pending_fullscreen = fullscreen;
                save();
            }
            return true;
        }
        if (menu) {
            if (wp == VK_UP) selected = (selected+row_count-1)%row_count;
            if (wp == VK_DOWN) selected = (selected+1)%row_count;
            if (wp == VK_LEFT || wp == VK_RIGHT) change(wp == VK_LEFT ? -1 : 1);
            if (wp == VK_RETURN && !repeat) activate(hwnd);
            return true;
        }
    }
    if (menu && (message == WM_KEYUP || message == WM_SYSKEYUP)) return true;
    if (menu && (message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE)) {
        layout(hwnd);
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < row_count; ++i) if (PtInRect(&rows[i], point)) {
            selected = i;
            if (message == WM_LBUTTONDOWN) {
                if (i < 5) change(point.x < (rows[i].left+rows[i].right)/2 ? -1 : 1);
                else activate(hwnd);
            }
            break;
        }
        return true;
    }
    return false;
}

// Compositing geometry is shared with the regression tests.
RECT game_rect(int w, int h) {
    int aw = widescreen ? 16 : 256;
    int ah = widescreen ? 9 : 240-overscan*2;
    int dw = w, dh = (int)((long long)w*ah/aw);
    if (dh > h) { dh = h; dw = (int)((long long)h*aw/ah); }
    return {(w-dw)/2, (h-dh)/2, (w+dw)/2, (h+dh)/2};
}

static void free_present() {
    if (present_dc) {
        SelectObject(present_dc, old_present_bitmap);
        DeleteObject(present_bitmap);
        DeleteDC(present_dc);
    }
    present_dc = nullptr; present_bitmap = nullptr;
    present_w = present_h = 0;
}

static bool client_surface(HDC dc, int w, int h) {
    if (present_dc && present_w == w && present_h == h) return true;
    HDC next_dc = CreateCompatibleDC(dc);
    BITMAPINFO bi={};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    void *bits=nullptr;
    HBITMAP next_bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!next_dc || !next_bitmap) {
        if (next_dc) DeleteDC(next_dc);
        if (next_bitmap) DeleteObject(next_bitmap);
        return false;
    }
    free_present();
    present_dc = next_dc; present_bitmap = next_bitmap;
    old_present_bitmap = SelectObject(present_dc, present_bitmap);
    present_w = w; present_h = h;
    return true;
}

static void video_render(HWND hwnd,HDC dc,const RECT &client,bool allow_gpu=false) {
    if (client.right<=0 || client.bottom<=0 || !output_dc) return;
    int w=resolutions[active].w,h=resolutions[active].h;
    RECT game=game_rect(w,h);
    bool hd=hd_active();
    int scale=hd ? HD_SCALE : 1;
    BITMAPINFO bi={};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=hd ? HD_WIDTH : 256;
    bi.bmiHeader.biHeight=-(hd ? HD_HEIGHT : 240);
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    int vw=client.right,vh=(int)((long long)vw*h/w);
    if (vh>client.bottom) { vh=client.bottom; vw=(int)((long long)vh*w/h); }
    int ox=(client.right-vw)/2,oy=(client.bottom-vh)/2;
    RECT dest={ox+int((long long)game.left*vw/w),oy+int((long long)game.top*vh/h),
               ox+int((long long)game.right*vw/w),oy+int((long long)game.bottom*vh/h)};
    bool notice=notification[0] && GetTickCount64()<notification_until;
    bool buffered=menu || notice;
    if (allow_gpu && !buffered && gpu_present(hwnd,hd ? hd_framebuffer() : ppu_fb,
        bi.bmiHeader.biWidth,-bi.bmiHeader.biHeight,dest,client,overscan*scale,(240-2*overscan)*scale)) return;
    if (buffered && !client_surface(dc,client.right,client.bottom)) return;
    HDC target=buffered ? present_dc : dc;
    // Never erase the game area. The four outside rectangles are only bars.
    fill(target,RECT{0,0,client.right,dest.top},RGB(0,0,0));
    fill(target,RECT{0,dest.bottom,client.right,client.bottom},RGB(0,0,0));
    fill(target,RECT{0,dest.top,dest.left,dest.bottom},RGB(0,0,0));
    fill(target,RECT{dest.right,dest.top,client.right,dest.bottom},RGB(0,0,0));
    SetStretchBltMode(target,COLORONCOLOR);
    StretchDIBits(target,dest.left,dest.top,dest.right-dest.left,dest.bottom-dest.top,
        0,overscan*scale,bi.bmiHeader.biWidth,(240-2*overscan)*scale,
        hd ? hd_framebuffer() : ppu_fb,&bi,DIB_RGB_COLORS,SRCCOPY);
    if (notice) {
        RECT toast={20,client.bottom-74,client.right-20,client.bottom-18};
        fill(target,toast,notification_error ? RGB(70,24,28) : RGB(16,23,39));
        ui_scale=1.0;
        RECT label=toast; label.left+=16; label.right-=16;
        text(target,notification,label,RGB(245,231,192),16,true);
    }
    if (menu) draw_menu(hwnd,target);
    if (buffered) BitBlt(dc,0,0,client.right,client.bottom,present_dc,0,0,SRCCOPY);
}

void video_present(HWND hwnd) {
    HDC dc=GetDC(hwnd);
    RECT client; GetClientRect(hwnd,&client);
    video_render(hwnd,dc,client,true);
    ReleaseDC(hwnd,dc);
}

void video_shutdown() { gpu_shutdown(); free_present(); free_surface(); }

void video_notify(const wchar_t *message,bool error) {
    wcsncpy_s(notification,message,_TRUNCATE);
    notification_error=error;
    notification_until=GetTickCount64()+4500;
}
