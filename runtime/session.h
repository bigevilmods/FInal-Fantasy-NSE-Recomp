#pragma once
#include <windows.h>
void session_init();
bool session_key(WPARAM key, LPARAM flags);
void session_save_resume();
