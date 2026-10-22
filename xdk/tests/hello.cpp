// Smallest useful Xbox 360 SDK program: proves that the compiler, linker and
// imagexex produce a .xex that runs from the dashboard. Clears the screen,
// A cycles the colour, Back returns to the dashboard.
#include <xtl.h>

static const D3DCOLOR colors[] = {
    D3DCOLOR_XRGB(91, 141, 239),   // Moonlight blue
    D3DCOLOR_XRGB(93, 184, 46),    // A green
    D3DCOLOR_XRGB(224, 69, 58),    // B red
    D3DCOLOR_XRGB(232, 182, 35),   // Y yellow
};

void __cdecl main()
{
    Direct3D *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DDevice *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    DWORD prev_buttons = 0;
    int color = 0;

    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (d3d == NULL ||
        FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)))
        return;

    for (;;) {
        XINPUT_STATE state;
        DWORD buttons = 0;

        if (XInputGetState(0, &state) == ERROR_SUCCESS)
            buttons = state.Gamepad.wButtons;
        if ((buttons & XINPUT_GAMEPAD_A) && !(prev_buttons & XINPUT_GAMEPAD_A))
            color = (color + 1) % (sizeof(colors) / sizeof(colors[0]));
        if (buttons & XINPUT_GAMEPAD_BACK)
            XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
        prev_buttons = buttons;

        device->Clear(0, NULL, D3DCLEAR_TARGET, colors[color], 1.0f, 0);
        device->Present(NULL, NULL, NULL, NULL);
    }
}
