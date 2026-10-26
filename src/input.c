#include "input.h"

#include <string.h>
#include <input/input.h>
#include <usb/usbmain.h>
#include <Limelight.h>
#include "../compat/xthread.h"

#define QUIT_COMBO (BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG)
#define SEND_INTERVAL_US 4000

static struct controller_data_s raw[MAX_PADS];
static bool seen[MAX_PADS];
static int menu_prev;

static volatile bool stream_active;
static volatile bool quit_requested;
static xthread_t *stream_thread;

int input_poll(struct pad pads[MAX_PADS])
{
    int i, mask = 0;

    for (i = 0; i < MAX_PADS; i++) {
        struct controller_data_s d;
        struct pad *p = &pads[i];

        /* The driver only flags reports that changed; keep the last state. */
        if (get_controller_data(&d, i)) {
            raw[i] = d;
            seen[i] = true;
        }
        memset(p, 0, sizeof(*p));
        if (!seen[i])
            continue;

        d = raw[i];
        p->present = true;
        mask |= 1 << i;
        p->buttons = (d.a ? A_FLAG : 0) | (d.b ? B_FLAG : 0) | (d.x ? X_FLAG : 0) | (d.y ? Y_FLAG : 0) |
                     (d.up ? UP_FLAG : 0) | (d.down ? DOWN_FLAG : 0) | (d.left ? LEFT_FLAG : 0) |
                     (d.right ? RIGHT_FLAG : 0) | (d.lb ? LB_FLAG : 0) | (d.rb ? RB_FLAG : 0) |
                     (d.start ? PLAY_FLAG : 0) | (d.back ? BACK_FLAG : 0) |
                     (d.s1_z ? LS_CLK_FLAG : 0) | (d.s2_z ? RS_CLK_FLAG : 0) |
                     (d.logo ? SPECIAL_FLAG : 0);
        p->lt = d.lt;
        p->rt = d.rt;
        p->lx = d.s1_x;
        p->ly = d.s1_y;
        p->rx = d.s2_x;
        p->ry = d.s2_y;
    }
    return mask;
}

int input_menu_pressed(void)
{
    struct pad pads[MAX_PADS];
    int i, now = 0, pressed;

    /* Drain pending USB reports first: after a slow screen redraw the driver
     * may still hold a button release that hasn't been processed yet. */
    usb_do_poll();
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
     * Holding a direction (or LB/RB) repeats it, like a keyboard. Only while
     * the menu is polling continuously: if the previous poll was long ago
     * (the screen was busy redrawing), restart the hold timer instead, so a
     * slow redraw can never turn one press into endless scrolling.
     */
    {
        const int repeatable = UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG | RB_FLAG;
        static uint64_t held_since, last_repeat, last_poll;
        static int held;
        uint64_t t = xthread_now_us();
        int dir = now & repeatable;
        bool gap = t - last_poll > 100000;

        last_poll = t;
        if (dir != held || gap) {
            held = dir;
            held_since = last_repeat = t;
        } else if (dir && t - held_since > 400000 && t - last_repeat > 110000) {
            pressed |= dir;
            last_repeat = t;
        }
    }

    menu_prev = now;
    return pressed;
}

static void *input_stream_main(void *arg)
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
                LiSendControllerArrivalEvent(i, mask, LI_CTYPE_XBOX,
                    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                    LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG,
                    LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);
                announced[i] = true;
            }
            if ((p->buttons & QUIT_COMBO) == QUIT_COMBO) {
                quit_requested = true;
                continue;
            }
            if (memcmp(p, &last[i], sizeof(*p)) != 0) {
                LiSendMultiControllerEvent(i, mask, p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
                last[i] = *p;
            }
        }
        xthread_sleep_us(SEND_INTERVAL_US);
    }
    return NULL;
}

void input_stream_start(void)
{
    quit_requested = false;
    stream_active = true;
    stream_thread = xthread_create("input", input_stream_main, NULL, 0);
}

void input_stream_stop(void)
{
    int i;

    stream_active = false;
    if (stream_thread) {
        xthread_join(stream_thread, NULL);
        stream_thread = NULL;
    }
    for (i = 0; i < MAX_PADS; i++)
        set_controller_rumble(i, 0, 0);
    /* Swallow buttons still held from the quit combo. */
    menu_prev = ~0;
}

bool input_quit_requested(void)
{
    return quit_requested;
}

int input_overlay_toggles(void)
{
    return 0;   /* this build has no performance overlay (video_has_overlay) */
}

void input_rumble(unsigned short controller, unsigned short low, unsigned short high)
{
    if (controller < MAX_PADS)
        set_controller_rumble(controller, (uint8_t)(low >> 8), (uint8_t)(high >> 8));
}
