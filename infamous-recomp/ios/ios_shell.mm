/* iOS / iPadOS app shell for the inFamous port (Apple port).
 *
 * On Windows and macOS the game's main() owns the process: it starts the frame
 * clock and runs the guest on the main thread. iOS gives the main thread to
 * UIKit, so this shell owns it instead:
 *
 *   - SDL3 starts UIKit (SDL_main.h) and calls main() below on the main thread.
 *   - It makes a full-screen Metal view and hands its CAMetalLayer to the Metal
 *     backend (rsx_metal_backend_set_layer), which letterboxes the game into it.
 *   - It runs the game's own main (port_game_main in main.cpp) on a thread with
 *     a big stack, and pumps SDL events here (gamepads, lifecycle).
 *   - Going to the background it parks the frame clock and drains the GPU
 *     queue: iOS stops an app that runs GPU work in the background.
 *   - It logs os_proc_available_memory() every few seconds: iOS kills an app
 *     that runs out without warning.
 *   - It puts an on-screen controller over the game (touch_pad.mm), shown
 *     while no gamepad is connected.
 *
 * Game data is never in the bundle. The ELF path comes from the first launch
 * argument (Simulator: a host path) or from Documents/BCES00220 (main.cpp
 * find_default_elf). The bundle carries launch.env, the launcher environment
 * from installer/infamous.toml; anything already set (SIMCTL_CHILD_*) wins.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <os/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int  port_game_main(int argc, char** argv);   /* main.cpp */
extern "C" void port_app_set_paused(int paused);         /* main.cpp */
extern "C" int  port_app_rsx_parked(void);               /* main.cpp */
extern "C" void rsx_metal_backend_set_layer(void* ca_metal_layer);
extern "C" void rsx_metal_backend_wait_idle(void);
extern "C" void port_touch_pad_attach(void* uiwindow);   /* touch_pad.mm */
extern "C" void port_touch_pad_gamepads(int count);     /* touch_pad.mm, main thread */

static void update_touch_pad(void)
{
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_free(ids);
    dispatch_async(dispatch_get_main_queue(), ^{ port_touch_pad_gamepads(n); });
}

static int    s_game_argc;
static char** s_game_argv;

static UIWindow*     s_uiwin;
static CAMetalLayer* s_layer;

/* Main thread. One line with every size that decides the picture's layout. */
static void log_geometry(const char* why)
{
    if (!s_uiwin) return;
    UIView*        v   = s_uiwin.rootViewController.view;
    UIWindowScene* sc  = s_uiwin.windowScene;
    const CGRect   sb  = UIScreen.mainScreen.bounds;
    const CGRect   scb = sc ? sc.coordinateSpace.bounds : CGRectZero;
    const CGRect   wf  = s_uiwin.frame;
    const CGRect   vb  = v ? v.bounds : CGRectZero;
    const CGSize   ds  = s_layer ? s_layer.drawableSize : CGSizeZero;
    const long     io  = sc ? (long)sc.effectiveGeometry.interfaceOrientation : -1;
    fprintf(stderr, "[ios-geo] %s: screen %.0fx%.0f, scene %.0fx%.0f (orientation %ld), window %.0fx%.0f, "
                    "view %.0fx%.0f, drawable %.0fx%.0f, scale %.2f native %.2f\n",
            why, sb.size.width, sb.size.height, scb.size.width, scb.size.height, io,
            wf.size.width, wf.size.height, vb.size.width, vb.size.height, ds.width, ds.height,
            UIScreen.mainScreen.scale, UIScreen.mainScreen.nativeScale);
}

/* A normal launch (home screen, devicectl) gives the app /dev/null for its
 * output; the log then goes to Documents, like the desktop launcher's
 * play.out.log / play.err.log. A console (simctl --console-pty) or a file
 * (simctl --stdout/--stderr) is left alone. */
static void redirect_logs(NSString* docs)
{
    struct stat st;
    if (fstat(STDERR_FILENO, &st) != 0 || !S_ISCHR(st.st_mode) || isatty(STDERR_FILENO))
        return;
    NSString* out = [docs stringByAppendingPathComponent:@"play.out.log"];
    NSString* err = [docs stringByAppendingPathComponent:@"play.err.log"];
    freopen(out.fileSystemRepresentation, "w", stdout);
    freopen(err.fileSystemRepresentation, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void load_launch_env(void)
{
    NSString* path = [[NSBundle mainBundle] pathForResource:@"launch" ofType:@"env"];
    FILE* f = path ? fopen(path.fileSystemRepresentation, "r") : NULL;
    if (!f) { fprintf(stderr, "[ios] no launch.env in the bundle\n"); return; }
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 0);
        n++;
    }
    fclose(f);
    fprintf(stderr, "[ios] launch.env: %d variables (existing values kept)\n", n);
}

static void* memory_logger(void*)
{
    pthread_setname_np("ios-mem");
    const char* e = getenv("IOS_MEM_LOG_SECS");
    int secs = (e && *e) ? atoi(e) : 5;
    if (secs < 1) secs = 1;
    for (;;) {
        fprintf(stderr, "[ios-mem] available %zu MB\n", os_proc_available_memory() >> 20);
        sleep((unsigned)secs);
    }
    return NULL;
}

static void* game_thread(void*)
{
    pthread_setname_np("game-main");
    int rc = port_game_main(s_game_argc, s_game_argv);
    fprintf(stderr, "[ios] game main returned %d\n", rc);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
    return NULL;
}

/* Runs on the main thread, inside the UIKit callback, before it returns:
 * iOS gives the app only a moment once it has left the screen. */
static bool SDLCALL app_events(void*, SDL_Event* e)
{
    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: {
        port_app_set_paused(1);
        int waited = 0;
        while (!port_app_rsx_parked() && waited < 2000) { usleep(5000); waited += 5; }
        rsx_metal_backend_wait_idle();
        fprintf(stderr, "[ios] background: frame clock %s after %d ms, GPU idle\n",
                port_app_rsx_parked() ? "parked" : "NOT parked", waited);
        break;
    }
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        port_app_set_paused(0);
        fprintf(stderr, "[ios] foreground: resumed\n");
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        update_touch_pad();
        break;
    case SDL_EVENT_LOW_MEMORY:
        fprintf(stderr, "[ios-mem] LOW MEMORY warning, available %zu MB\n",
                os_proc_available_memory() >> 20);
        break;
    case SDL_EVENT_TERMINATING:
        fprintf(stderr, "[ios] terminating\n");
        break;
    default:
        break;
    }
    return true;
}

int main(int argc, char* argv[])
{
    @autoreleasepool {
        NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        redirect_logs(docs);
        setvbuf(stderr, NULL, _IONBF, 0);
        /* settings.ini and anything else the game writes next to itself. */
        chdir(docs.fileSystemRepresentation);

        NSProcessInfo* pi = [NSProcessInfo processInfo];
        fprintf(stderr, "[ios] %s, %lu cores, %llu MB RAM, %zu MB available to the app\n",
                pi.operatingSystemVersionString.UTF8String, (unsigned long)pi.activeProcessorCount,
                pi.physicalMemory >> 20, os_proc_available_memory() >> 20);
        fprintf(stderr, "[ios] documents: %s\n", docs.fileSystemRepresentation);

        load_launch_env();

        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
        /* Landscape only. Without this, SDL's view-frame code (UIKit_ComputeViewFrame) reads the old
         * statusBarOrientation, which on iPadOS 27 says portrait, and swaps the view to 970x1408 on the
         * first resume: the picture and touch pad were laid out portrait on a landscape screen. */
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
            fprintf(stderr, "[ios] SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_AddEventWatch(app_events, NULL);

        SDL_Window* win = SDL_CreateWindow("inFamous", 0, 0,
            SDL_WINDOW_FULLSCREEN | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
        SDL_MetalView view = win ? SDL_Metal_CreateView(win) : NULL;
        CAMetalLayer* layer = view ? (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view) : nil;
        if (layer) {
            layer.device          = MTLCreateSystemDefaultDevice();
            layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = YES;
            layer.opaque          = YES;
            rsx_metal_backend_set_layer((__bridge void*)layer);
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(win, &pw, &ph);
            fprintf(stderr, "[ios] Metal view %dx%d px\n", pw, ph);
        } else {
            fprintf(stderr, "[ios] no Metal view (%s): rendering headless\n", SDL_GetError());
        }
        void* uiwin = win ? SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                   SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL) : NULL;
        if (uiwin) {
            port_touch_pad_attach(uiwin);
            update_touch_pad();
            s_uiwin = (__bridge UIWindow*)uiwin;
            s_layer = layer;
            log_geometry("start");
        }

        pthread_t mt;
        pthread_create(&mt, NULL, memory_logger, NULL);
        pthread_detach(mt);

        /* The desktop build runs the guest on the process main thread (8 MB
         * on macOS, more on Windows); iOS gives other threads 512 KB. */
        s_game_argc = argc;
        s_game_argv = argv;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_t gt;
        if (pthread_create(&gt, &attr, game_thread, NULL) != 0) {
            fprintf(stderr, "[ios] could not start the game thread\n");
            return 1;
        }
        pthread_attr_destroy(&attr);
    }

    const Uint64 t0 = SDL_GetTicks();
    int geo_step = 0;
    for (;;) {
        @autoreleasepool {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 100)) {
                if (e.type == SDL_EVENT_QUIT) break;
                switch (e.type) {
                case SDL_EVENT_WINDOW_RESIZED:            log_geometry("window resized"); break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: log_geometry("pixel size changed"); break;
                case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED: log_geometry("metal view resized"); break;
                case SDL_EVENT_DISPLAY_ORIENTATION:       log_geometry("display orientation"); break;
                default: break;
                }
            }
            static const Uint64 geo_at[] = { 1000, 3000, 10000 };
            if (geo_step < 3 && SDL_GetTicks() - t0 >= geo_at[geo_step]) {
                char b[32]; snprintf(b, sizeof b, "t=%llus", (unsigned long long)geo_at[geo_step] / 1000);
                log_geometry(b);
                geo_step++;
            }
        }
    }
    fprintf(stderr, "[ios] quit\n");
    exit(0);
}
