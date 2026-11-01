#pragma once
#include <windows.h>

void video_init(HWND hwnd);
bool video_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
bool video_menu_open();
void video_present(HWND hwnd);
void video_shutdown();
void video_notify(const wchar_t *message, bool error = false);
