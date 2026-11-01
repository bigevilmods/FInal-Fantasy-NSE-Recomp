#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "session.h"
#include "state.h"
#include "nes.h"
#include "video.h"

static int slot=1;
static char preferences[MAX_PATH];
static void notify_error() {
    wchar_t text[256];
    MultiByteToWideChar(CP_UTF8,0,state_error(),-1,text,256);
    video_notify(text,true);
}
static void notify_slot(const wchar_t *action) {
    wchar_t text[256]; swprintf_s(text,L"%s — slot %d / 10  |  F5 save · F9 load · F6 change",action,slot);
    video_notify(text);
}
void session_init() {
    CreateDirectoryA("states",nullptr);
    GetFullPathNameA("states/session.ini",MAX_PATH,preferences,nullptr);
    slot=(int)GetPrivateProfileIntA("Session","Slot",1,preferences);
    if (slot<1 || slot>10) slot=1;
    if (GetFileAttributesA("states/resume.ffstate")!=INVALID_FILE_ATTRIBUTES) {
        if (state_load_file("states/resume.ffstate")) {
            controller_state=0;
            video_notify(L"Last restored session. F5 save · F9 load · F6 change slot");
        } else notify_error();
    } else notify_slot(L"Save states prontos");
}
bool session_key(WPARAM key,LPARAM flags) {
    if (key!=VK_F5 && key!=VK_F6 && key!=VK_F8 && key!=VK_F9) return false;
    if (flags&(1LL<<30)) return true;
    if (key==VK_F6) {
        slot=slot%10+1;
        char text[16]; sprintf_s(text,"%d",slot);
        WritePrivateProfileStringA("Session","Slot",text,preferences);
        notify_slot(L"Slot selected"); return true;
    }
    char path[100]; sprintf_s(path,"states/slot%02d.ffstate",slot);
    if (key==VK_F5) {
        if (state_save_file(path)) notify_slot(L"State saved"); else notify_error();
    } else {
        if (key==VK_F8) strcpy_s(path,"states/resume.ffstate");
        bool paused=video_menu_open();
        apu_audio_stop();
        bool ok=state_load_file(path);
        if (ok) { controller_state=0; notify_slot(L"state loaded"); }
        else notify_error();
        if (!paused) apu_audio_start();
    }
    return true;
}
void session_save_resume() {
    if (nes_frame>0 && !cpu_fault && !state_save_file("states/resume.ffstate"))
        MessageBoxA(nullptr,state_error(),"fail on load last session",MB_OK|MB_ICONERROR);
}
