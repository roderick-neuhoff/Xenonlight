#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_PADS 4

struct pad {
    bool present;
    int buttons;            /* Moonlight *_FLAG bits */
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};

/* Refreshes pads[] from the USB controller driver. Returns present mask. */
int input_poll(struct pad pads[MAX_PADS]);

/* Edge-detected button presses on any controller, for menus. */
int input_menu_pressed(void);

/* Starts/stops the coroutine that forwards controller state to the host. */
void input_stream_start(void);
void input_stream_stop(void);

/* True once the user held Back+Start+LB+RB to leave the stream. */
bool input_quit_requested(void);

void input_rumble(unsigned short controller, unsigned short low, unsigned short high);
