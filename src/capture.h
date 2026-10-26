#pragma once
#include <d3d9.h>

// Hooks D3D9 Present/Reset (via the vtables of throwaway devices, so it works no matter when or
// through which wrapper the game creates its device) and encodes swapchain 0 with our own NVENC.
void capture_install();

// The game's recorder handed this surface to NVENC: it holds the frame without the elements the
// recorder hides (credits, arcade name, ...). Used as the source while it stays fresh.
// The game runs more than one recorder encoder per song; only the largest one is followed.
void capture_game_frame(IDirect3DSurface9* s, void* encoder);
void capture_forget_game_surface(IDirect3DSurface9* s);

// The game destroyed one of its NVENC sessions. The driver then hands our session stale copies of
// its D3D9 inputs, so our input registrations are refreshed before the next frame.
void capture_game_session_closed(void* encoder);
