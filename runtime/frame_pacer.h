#pragma once
#include <windows.h>
#include <mmsystem.h>

class FramePacer {
    HANDLE timer=nullptr;
    bool precise_period=false;
public:
    FramePacer() {
        precise_period=timeBeginPeriod(1)==TIMERR_NOERROR;
        // High-resolution waitable timer on supported Windows versions.
        timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if (!timer) timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }
    ~FramePacer() {
        if (timer) CloseHandle(timer);
        if (precise_period) timeEndPeriod(1);
    }
    void sleep_for(double seconds) {
        if (seconds<=0.0001) return;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart=-(LONGLONG)(seconds*10000000.0);
            if (SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)) {
                WaitForSingleObject(timer,INFINITE); return;
            }
        }
        DWORD ms=(DWORD)(seconds*1000.0);
        if (ms) Sleep(ms);
    }
};
