/*
 * ps3recomp - keyboard + mouse fed by the host app (macOS)
 *
 * On Windows cellPad.c reads the keyboard and mouse itself (GetAsyncKeyState,
 * raw input from the D3D12 window). On macOS the app's AppKit event monitors
 * report them here instead (infamous-recomp/mac/mac_kbm.mm), and cellPad.c
 * runs the same keyboard + mouse pad as Windows (pad_poll_kbm, PAD_KBM).
 *
 * Keys are given as Windows virtual-key codes, so both platforms share one
 * set of bindings. Call from any thread (the app calls from the main thread,
 * the game reads on its own thread).
 */
#ifndef PS3RECOMP_PAD_HOST_KBM_H
#define PS3RECOMP_PAD_HOST_KBM_H

#ifdef __cplusplus
extern "C" {
#endif

/* The Windows virtual-key codes the bindings use. */
enum {
    PAD_VK_LBUTTON = 0x01, PAD_VK_RBUTTON = 0x02, PAD_VK_MBUTTON = 0x04,
    PAD_VK_BACK    = 0x08, PAD_VK_TAB     = 0x09, PAD_VK_RETURN  = 0x0D,
    PAD_VK_SHIFT   = 0x10, PAD_VK_CONTROL = 0x11, PAD_VK_MENU    = 0x12,   /* MENU = Alt (Option on a Mac) */
    PAD_VK_ESCAPE  = 0x1B, PAD_VK_SPACE   = 0x20,
    PAD_VK_LEFT    = 0x25, PAD_VK_UP      = 0x26, PAD_VK_RIGHT   = 0x27, PAD_VK_DOWN = 0x28
    /* letters: their upper-case ASCII code ('W', 'A', ...), as on Windows */
};

/* A key or mouse button went down (1) or up (0). */
void ps3_pad_host_key(int vk, int down);
/* The game window has keyboard focus (1) or lost it (0). Losing it releases every key. */
void ps3_pad_host_focus(int focused);
/* Mouse movement in device units since the last call (x right, y down). */
void ps3_pad_host_mouse_move(long dx, long dy);
/* Mouse wheel (+ = away from the player). */
void ps3_pad_host_wheel(long w);
/* 1 while the game wants the pointer hidden and held (mouse look), as on Windows. */
int  ps3_pad_host_mouse_wanted(void);
/* 0 when keyboard + mouse are off (PAD_KBM=0 or PAD_NO_KEYBOARD set). */
int  ps3_pad_host_kbm_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PAD_HOST_KBM_H */
