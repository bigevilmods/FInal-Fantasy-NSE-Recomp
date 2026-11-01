#pragma once
#include <windows.h>
#include <stdint.h>
bool gpu_present(HWND hwnd,const uint32_t *pixels,int width,int height,
                 const RECT &destination,const RECT &client,int crop_top,int crop_height,
                 bool capture_for_test=false);
void gpu_shutdown();
bool gpu_readback(uint32_t *pixels,int width,int height);
