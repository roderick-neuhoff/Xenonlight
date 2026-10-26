/*
 * Controllers through XInput: wired and wireless pads, up to four, plus USB
 * keyboards during a stream.
 *
 * Mouse mode (as in Moonlight-qt): holding Start alone for a second switches
 * a pad between controller and mouse, confirmed by a short rumble. In mouse
 * mode the left stick moves the pointer, the right stick scrolls, A/B/X are
 * the left/right/middle buttons and LB/RB are back/forward.
 */
#include "xdk.h"

#include <string.h>

#include "../../src/input.h"

#include <Limelight.h>

#define QUIT_COMBO (BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG)
#define OVERLAY_COMBO (BACK_FLAG | LB_FLAG | RB_FLAG | X_FLAG)
#define SEND_INTERVAL_MS 4

static int menu_prev;
static volatile bool stream_active;
static volatile bool quit_requested;
static volatile int overlay_toggles;
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

/* ------------------------------------------------------------ mouse mode */

#define MOUSE_TOGGLE_MS 1000
#define STICK_DEADZONE  7849       /* XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE */
#define POINTER_SPEED   1400.0f    /* pixels per second at full tilt */
#define SCROLL_SPEED    1200.0f    /* wheel units (120 = one notch) per second */

struct mouse_pad {
    bool on;
    bool holding;           /* Start alone is held ... */
    bool toggled;           /* ... and this hold already switched modes */
    DWORD held_since;
    DWORD rumble_off;       /* end of the confirmation rumble, 0 if none */
    int buttons;            /* pad flags of the mouse buttons currently down */
    float fx, fy, fv, fh;   /* sub-pixel and sub-unit remainders */
};

static const struct {
    int flag;
    int button;
} mouse_buttons[] = {
    { A_FLAG, BUTTON_LEFT },
    { B_FLAG, BUTTON_RIGHT },
    { X_FLAG, BUTTON_MIDDLE },
    { LB_FLAG, BUTTON_X1 },
    { RB_FLAG, BUTTON_X2 },
};
#define MOUSE_BUTTON_FLAGS (A_FLAG | B_FLAG | X_FLAG | LB_FLAG | RB_FLAG)

/* Stick position to -1..1 outside the dead zone, squared for fine control near the centre. */
static float stick(short v)
{
    float a = v < 0 ? -(float)v : (float)v, n;

    if (a <= STICK_DEADZONE)
        return 0;
    n = (a - STICK_DEADZONE) / (32767.0f - STICK_DEADZONE);
    if (n > 1)
        n = 1;
    n *= n;
    return v < 0 ? -n : n;
}

/* Adds rate * dt to *acc and returns (and removes) its whole part. */
static int take(float *acc, float rate, float dt)
{
    int whole;

    *acc += rate * dt;
    whole = (int)*acc;
    *acc -= whole;
    return whole;
}

static void mouse_set_buttons(struct mouse_pad *m, int want)
{
    unsigned b;

    for (b = 0; b < sizeof(mouse_buttons) / sizeof(mouse_buttons[0]); b++) {
        int f = mouse_buttons[b].flag;
        if ((want ^ m->buttons) & f)
            LiSendMouseButtonEvent((want & f) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                                   mouse_buttons[b].button);
    }
    m->buttons = want;
}

/* Watches for the Start hold; returns true when the pad just switched modes. */
static bool mouse_toggle(struct mouse_pad *m, unsigned short pad, int buttons, DWORD now)
{
    if (m->rumble_off && (int)(now - m->rumble_off) >= 0) {
        input_rumble(pad, 0, 0);
        m->rumble_off = 0;
    }
    if (buttons != PLAY_FLAG) {
        m->holding = false;
        return false;
    }
    if (!m->holding) {
        m->holding = true;
        m->toggled = false;
        m->held_since = now;
    }
    if (m->toggled || now - m->held_since < MOUSE_TOGGLE_MS)
        return false;
    m->toggled = true;
    m->on = !m->on;
    if (!m->on)
        mouse_set_buttons(m, 0);
    m->fx = m->fy = m->fv = m->fh = 0;
    /* Long buzz for mouse, short one for controller. */
    input_rumble(pad, 0x6000, 0x6000);
    m->rumble_off = (now + (m->on ? 250 : 100)) | 1;
    return true;
}

static void mouse_send(struct mouse_pad *m, const struct pad *p, float dt)
{
    int dx = take(&m->fx, stick(p->lx) * POINTER_SPEED, dt);
    int dy = take(&m->fy, -stick(p->ly) * POINTER_SPEED, dt);
    int v = take(&m->fv, stick(p->ry) * SCROLL_SPEED, dt);
    int h = take(&m->fh, stick(p->rx) * SCROLL_SPEED, dt);

    mouse_set_buttons(m, p->buttons & MOUSE_BUTTON_FLAGS);
    if (dx || dy)
        LiSendMouseMoveEvent((short)dx, (short)dy);
    if (v)
        LiSendHighResScrollEvent((short)v);
    if (h)
        LiSendHighResHScrollEvent((short)h);
}

/* ------------------------------------------------------------ keyboard */

/* USB keyboards through XInput's keystroke queue. Its virtual-key codes are the Windows ones
 * Moonlight sends (as 0x8000 | VK, like Moonlight-qt); key repeat is left to the host. */
static void keyboard_send(void)
{
    XINPUT_KEYSTROKE k;
    int n;

    for (n = 0; n < 32 && XInputGetKeystroke(XUSER_INDEX_ANY, XINPUT_FLAG_KEYBOARD, &k) == ERROR_SUCCESS; n++) {
        char mods = 0;

        if (k.VirtualKey == 0 || (k.Flags & XINPUT_KEYSTROKE_REPEAT) ||
            !(k.Flags & (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_KEYUP)))
            continue;
        if (k.Flags & XINPUT_KEYSTROKE_SHIFT)
            mods |= MODIFIER_SHIFT;
        if (k.Flags & XINPUT_KEYSTROKE_CTRL)
            mods |= MODIFIER_CTRL;
        if (k.Flags & (XINPUT_KEYSTROKE_ALT | XINPUT_KEYSTROKE_ALTGR))
            mods |= MODIFIER_ALT;
        LiSendKeyboardEvent((short)(0x8000 | (k.VirtualKey & 0xFF)),
                            (k.Flags & XINPUT_KEYSTROKE_KEYDOWN) ? KEY_ACTION_DOWN : KEY_ACTION_UP, mods);
    }
}

/* ------------------------------------------------------------ streaming */

static DWORD WINAPI input_stream_main(LPVOID arg)
{
    struct pad pads[MAX_PADS], last[MAX_PADS];
    struct mouse_pad mouse[MAX_PADS];
    bool announced[MAX_PADS] = { false }, overlay_held[MAX_PADS] = { false };
    DWORD prev = GetTickCount();
    int i;
    (void)arg;

    memset(last, 0, sizeof(last));
    memset(mouse, 0, sizeof(mouse));
    while (stream_active) {
        int mask = input_poll(pads);
        DWORD now = GetTickCount();
        float dt = (now - prev) / 1000.0f;

        prev = now;
        if (dt > 0.05f)
            dt = 0.05f;   /* after a stall, don't jump the pointer across the screen */
        for (i = 0; i < MAX_PADS; i++) {
            struct pad *p = &pads[i];
            struct mouse_pad *m = &mouse[i];

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
            /* The game still sees these buttons, as in Moonlight-qt. */
            if ((p->buttons & OVERLAY_COMBO) == OVERLAY_COMBO) {
                if (!overlay_held[i])
                    overlay_toggles++;
                overlay_held[i] = true;
            } else {
                overlay_held[i] = false;
            }
            if (mouse_toggle(m, (unsigned short)i, p->buttons, now)) {
                if (m->on) {
                    /* Release everything on the host's virtual pad (Start is still held). */
                    LiSendMultiControllerEvent((short)i, (short)mask, 0, 0, 0, 0, 0, 0, 0);
                } else {
                    /* The host's pad is still neutral; the next change (releasing Start) sends the real state. */
                    last[i] = *p;
                }
            }
            if (m->on) {
                mouse_send(m, p, dt);
                continue;
            }
            if (memcmp(p, &last[i], sizeof(*p)) != 0) {
                LiSendMultiControllerEvent((short)i, (short)mask, p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
                last[i] = *p;
            }
        }
        keyboard_send();
        Sleep(SEND_INTERVAL_MS);
    }
    return 0;
}

void input_stream_start(void)
{
    quit_requested = false;
    overlay_toggles = 0;
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

int input_overlay_toggles(void)
{
    return overlay_toggles;
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
