/* joyprobe — enumerate WinMM joysticks and live-sample input for 8 seconds.
 * Console app: xprun captures the output remotely. Wiggle the controller
 * while it runs; any axis/button change prints a line.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o joyprobe.exe joyprobe.c -lwinmm
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

int main(void)
{
    UINT n = joyGetNumDevs();
    printf("joyGetNumDevs = %u\n", n);

    int found = -1;
    for (UINT id = 0; id < n && id < 16; id++) {
        JOYINFOEX ji = { sizeof ji, JOY_RETURNALL };
        MMRESULT r = joyGetPosEx(id, &ji);
        if (r != JOYERR_NOERROR) continue;
        JOYCAPSA caps;
        joyGetDevCapsA(id, &caps, sizeof caps);
        printf("id %u ATTACHED: '%s'  axes=%u buttons=%u  "
               "x[%u..%u] y[%u..%u]\n",
               id, caps.szPname, caps.wNumAxes, caps.wNumButtons,
               caps.wXmin, caps.wXmax, caps.wYmin, caps.wYmax);
        if (found < 0) found = (int)id;
    }
    if (found < 0) {
        printf("NO JOYSTICK ATTACHED\n");
        return 1;
    }

    /* idle snapshot of every attached id: centered sticks read ~32767,
     * phantom interfaces tend to read 0 or 65535 on everything */
    JOYINFOEX last[16];
    int attached[16] = {0};
    for (UINT id = 0; id < n && id < 16; id++) {
        last[id].dwSize = sizeof last[id];
        last[id].dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &last[id]) == JOYERR_NOERROR) {
            attached[id] = 1;
            printf("idle id %u: x=%5lu y=%5lu z=%5lu r=%5lu pov=%5lu "
                   "btn=0x%04lx\n", id, last[id].dwXpos, last[id].dwYpos,
                   last[id].dwZpos, last[id].dwRpos, last[id].dwPOV,
                   last[id].dwButtons);
        }
    }

    FILE *log = fopen("joy.txt", "w");   /* mirror for remote reading */
    printf("\n"
           "  ############################################\n"
           "  ##                                        ##\n"
           "  ##   WIGGLE STICKS / PRESS BUTTONS NOW    ##\n"
           "  ##   sampling for 30 seconds...           ##\n"
           "  ##                                        ##\n"
           "  ############################################\n\n");
    fflush(stdout);

    DWORD t0 = GetTickCount();
    int lines = 0;
    while (GetTickCount() - t0 < 30000 && lines < 80) {
        for (UINT id = 0; id < n && id < 16; id++) {
            if (!attached[id]) continue;
            JOYINFOEX ji = { sizeof ji, JOY_RETURNALL };
            if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;
            int dx = (int)ji.dwXpos - (int)last[id].dwXpos;
            int dy = (int)ji.dwYpos - (int)last[id].dwYpos;
            int dz = (int)ji.dwZpos - (int)last[id].dwZpos;
            int dr = (int)ji.dwRpos - (int)last[id].dwRpos;
            if (ji.dwButtons != last[id].dwButtons ||
                dx > 2000 || dx < -2000 || dy > 2000 || dy < -2000 ||
                dz > 2000 || dz < -2000 || dr > 2000 || dr < -2000 ||
                ji.dwPOV != last[id].dwPOV) {
                printf("t=%5lums id=%u x=%5lu y=%5lu z=%5lu r=%5lu "
                       "pov=%5lu btn=0x%04lx\n",
                       GetTickCount() - t0, id, ji.dwXpos, ji.dwYpos,
                       ji.dwZpos, ji.dwRpos, ji.dwPOV, ji.dwButtons);
                if (log) {
                    fprintf(log, "t=%5lums id=%u x=%5lu y=%5lu z=%5lu "
                            "r=%5lu pov=%5lu btn=0x%04lx\n",
                            GetTickCount() - t0, id, ji.dwXpos, ji.dwYpos,
                            ji.dwZpos, ji.dwRpos, ji.dwPOV, ji.dwButtons);
                    fflush(log);
                }
                lines++;
                last[id] = ji;
            }
        }
        Sleep(40);
    }
    printf("done (%d change lines)\n", lines);
    if (log) {
        fprintf(log, "done (%d change lines)\n", lines);
        fclose(log);
    }
    return 0;
}
