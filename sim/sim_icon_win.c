/* The window's title bar and Alt-Tab icon: the one built into the .exe (skyframe.rc).
   Kept apart from sim_platform.c, whose ESP-IDF stand-ins don't mix with windows.h. */
#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

void sim_set_window_icon(SDL_Window *window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    HWND hwnd = info.info.win.window;
    HINSTANCE self = GetModuleHandleW(NULL);
    HICON big = (HICON)LoadImageW(self, L"SDL_app", IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON small = (HICON)LoadImageW(self, L"SDL_app", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
}
#else
typedef struct SDL_Window SDL_Window;
void sim_set_window_icon(SDL_Window *window) { (void)window; }
#endif
