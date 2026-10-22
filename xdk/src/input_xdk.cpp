/*
 * Controllers through XInput: wired and wireless pads, up to four.
 */
#include "xdk.h"

#include <string.h>

#include "../../src/input.h"

#include <Limelight.h>

#define QUIT_COMBO (BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG)
#define SEND_INTERVAL_MS 4

static int menu_prev;
static volatile bool stream_active;
static volatile bool quit_requested;
static HANDLE stream_thread;

static const struct {
    WORD xinput;
    int flag;
} button_map[] = {
    { XINPUT_GAMEPAD_A, A_FLAG },
    { XINPUT_GAMEPAD_B, B_FLAG },
    { XINPUT_GAMEPAD_X, X_FLAG },
    { XINPUT_GAMEPAD_Y, Y_FLAG },
    { XINPUT_GAMEPAD_DPAD_UP, UP_FLAG },
    { XINPUT_GAMEPAD_DPAD_DOWN, DOWN_FLAG },
    { XINPUT_GAMEPAD_DPAD_LEFT, LEFT_FLAG },
    { XINPUT_GAMEPAD_DPAD_RIGHT, RIGHT_FLAG },
    { XINPUT_GAMEPAD_LEFT_SHOULDER, LB_FLAG },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, RB_FLAG },
    { XINPUT_GAMEPAD_START, PLAY_FLAG },
    { XINPUT_GAMEPAD_BACK, BACK_FLAG },
    { XINPUT_GAMEPAD_LEFT_THUMB, LS_CLK_FLAG },
    { XINPUT_GAMEPAD_RIGHT_THUMB, RS_CLK_FLAG },
};

int input_poll(struct pad pads[MAX_PADS])
{
    int i, mask = 0;

    for (i = 0; i < MAX_PADS; i++) {
        struct pad *p = &pads[i];
        XINPUT_STATE s;
        unsigned b;

        memset(p, 0, sizeof(*p));
        if (XInputGetState(i, &s) != ERROR_SUCCESS)
            continue;
        p->present = true;
        mask |= 1 << i;
        for (b = 0; b < sizeof(button_map) / sizeof(button_map[0]); b++)
            if (s.Gamepad.wButtons & button_map[b].xinput)
                p->buttons |= button_map[b].flag;
        p->lt = s.Gamepad.bLeftTrigger;
        p->rt = s.Gamepad.bRightTrigger;
        p->lx = s.Gamepad.sThumbLX;
        p->ly = s.Gamepad.sThumbLY;
        p->rx = s.Gamepad.sThumbRX;
        p->ry = s.Gamepad.sThumbRY;
    }
    return mask;
}

int input_menu_pressed(void)
{
    const int repeatable = UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG | RB_FLAG;
    static DWORD held_since, last_repeat, last_poll;
    static int held;
    struct pad pads[MAX_PADS];
    int i, now = 0, pressed, dir;
    DWORD t = GetTickCount();
    bool gap;

    input_poll(pads);
    for (i = 0; i < MAX_PADS; i++) {
        if (!pads[i].present)
            continue;
        now |= pads[i].buttons;
        /* Treat stick flicks as d-pad presses in menus. */
        if (pads[i].ly > 20000) now |= UP_FLAG;
        if (pads[i].ly < -20000) now |= DOWN_FLAG;
        if (pads[i].lx < -20000) now |= LEFT_FLAG;
        if (pads[i].lx > 20000) now |= RIGHT_FLAG;
    }
    pressed = now & ~menu_prev;

    /*
     * Holding a direction (or LB/RB) repeats it. If the previous poll was
     * long ago (the screen was busy), restart the hold timer instead, so a
     * slow redraw can't turn one press into endless scrolling.
     */
    dir = now & repeatable;
    gap = t - last_poll > 100;
    last_poll = t;
    if (dir != held || gap) {
        held = dir;
        held_since = last_repeat = t;
    } else if (dir && t - held_since > 400 && t - last_repeat > 110) {
        pressed |= dir;
        last_repeat = t;
    }

    menu_prev = now;
    return pressed;
}

static DWORD WINAPI input_stream_main(LPVOID arg)
{
    struct pad pads[MAX_PADS], last[MAX_PADS];
    bool announced[MAX_PADS] = { false };
    int i;
    (void)arg;

    memset(last, 0, sizeof(last));
    while (stream_active) {
        int mask = input_poll(pads);

        for (i = 0; i < MAX_PADS; i++) {
            struct pad *p = &pads[i];

            if (!p->present)
                continue;
            if (!announced[i]) {
                LiSendControllerArrivalEvent((uint8_t)i, (uint16_t)mask, LI_CTYPE_XBOX,
                    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                    LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG,
                    LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);
                announced[i] = true;
            }
            if ((p->buttons & QUIT_COMBO) == QUIT_COMBO) {
                quit_requested = true;
                continue;
            }
            if (memcmp(p, &last[i], sizeof(*p)) != 0) {
                LiSendMultiControllerEvent((short)i, (short)mask, p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
                last[i] = *p;
            }
        }
        Sleep(SEND_INTERVAL_MS);
    }
    return 0;
}

void input_stream_start(void)
{
    quit_requested = false;
    stream_active = true;
    stream_thread = CreateThread(NULL, 64 * 1024, input_stream_main, NULL, 0, NULL);
}

void input_stream_stop(void)
{
    int i;

    stream_active = false;
    if (stream_thread) {
        WaitForSingleObject(stream_thread, INFINITE);
        CloseHandle(stream_thread);
        stream_thread = NULL;
    }
    for (i = 0; i < MAX_PADS; i++)
        input_rumble((unsigned short)i, 0, 0);
    /* Swallow buttons still held from the quit combo. */
    menu_prev = ~0;
}

bool input_quit_requested(void)
{
    return quit_requested;
}

void input_rumble(unsigned short controller, unsigned short low, unsigned short high)
{
    XINPUT_VIBRATION v;

    if (controller >= MAX_PADS)
        return;
    v.wLeftMotorSpeed = low;
    v.wRightMotorSpeed = high;
    XInputSetState(controller, &v);
}
