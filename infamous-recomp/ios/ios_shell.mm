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
 * find_default_elf). With neither, the first-run screen (ios_import.mm) asks
 * for the player's disc image or game folder and imports it there, then the
 * game starts. The bundle carries launch.env, the launcher environment
 * from installer/infamous.toml; anything already set (SIMCTL_CHILD_*) wins.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <mach/mach.h>
#include <os/proc.h>
#include <pthread/qos.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <string>

extern "C" int  port_game_main(int argc, char** argv);   /* main.cpp */
extern "C" void port_app_set_paused(int paused);         /* main.cpp */
extern "C" int  port_app_rsx_parked(void);               /* main.cpp */
extern "C" void rsx_metal_backend_set_layer(void* ca_metal_layer);
extern "C" void rsx_metal_backend_wait_idle(void);
extern "C" void port_touch_pad_attach(void* uiwindow);   /* touch_pad.mm */
extern "C" void port_touch_pad_gamepads(int count);     /* touch_pad.mm, main thread */
extern "C" void port_ios_import_show(void* uiwindow, const char* dest_root, void (^done)(void));   /* ios_import.mm */
extern "C" int  di_installed(const char* dest_root, char* elf_out, size_t elf_out_len);              /* import/disc_import.c */

/* CPU check. The iOS build uses -mcpu=apple-a14 (CMakeLists.txt, PORT_IOS_CPU),
 * so its code may use FRINT32/64 (FEAT_FRINTTS) and AXFLAG/XAFLAG (FEAT_FlagM2).
 * A13 and older lack both and would crash on the first one. iOS 16 still runs
 * on those chips and Info.plist can't ask for an A14, so check here and show a
 * message instead. INFAMOUS_CPU_TEST=old (testing) fakes an old chip. */
static bool cpu_supported(void)
{
#if defined(PORT_IOS_CPU_CHECK)
    if (const char* t = getenv("INFAMOUS_CPU_TEST"); t && !strcmp(t, "old")) {
        fprintf(stderr, "[ios] cpu: INFAMOUS_CPU_TEST=old\n");
        return false;
    }
    static const char* const need[] = { "hw.optional.arm.FEAT_FRINTTS", "hw.optional.arm.FEAT_FlagM2" };
    for (const char* n : need) {
        int v = 0;
        size_t len = sizeof v;
        if (sysctlbyname(n, &v, &len, NULL, 0) != 0) {
            fprintf(stderr, "[ios] cpu: no %s, not checked\n", n);   /* don't block on an unknown */
            continue;
        }
        if (!v) {
            fprintf(stderr, "[ios] cpu: %s = 0: chip older than A14\n", n);
            return false;
        }
    }
#endif
    return true;
}

@interface PortTooOldController : UIViewController
@end

@implementation PortTooOldController
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    UILabel* title = [UILabel new];
    title.font = [UIFont systemFontOfSize:34 weight:UIFontWeightBold];
    title.textColor = UIColor.whiteColor;
    title.text = @"inFamous";
    UILabel* body = [UILabel new];
    body.font = [UIFont systemFontOfSize:19 weight:UIFontWeightRegular];
    body.textColor = [UIColor colorWithWhite:0.85 alpha:1];
    body.numberOfLines = 0;
    body.textAlignment = NSTextAlignmentCenter;
    body.text = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
        ? @"Sorry, this iPad is too old to run the game.\n\n"
           "It needs an A14 or M1 chip or newer: iPad Air (4th generation), "
           "iPad (10th generation), iPad mini (6th generation), iPad Pro with M1, or newer."
        : @"Sorry, this iPhone is too old to run the game.\n\n"
           "It needs an iPhone 12 or newer.";
    UIStackView* st = [[UIStackView alloc] initWithArrangedSubviews:@[ title, body ]];
    st.axis = UILayoutConstraintAxisVertical;
    st.alignment = UIStackViewAlignmentCenter;
    st.spacing = 22;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:st];
    UILayoutGuide* g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [st.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
        [st.centerYAnchor constraintEqualToAnchor:g.centerYAnchor],
        [st.widthAnchor constraintLessThanOrEqualToConstant:620],
        [st.leadingAnchor constraintGreaterThanOrEqualToAnchor:g.leadingAnchor constant:24],
        [st.trailingAnchor constraintLessThanOrEqualToAnchor:g.trailingAnchor constant:-24],
        [body.widthAnchor constraintEqualToAnchor:st.widthAnchor],
    ]];
}
@end

static void show_too_old(UIWindow* win)
{
    PortTooOldController* c = [PortTooOldController new];
    c.modalPresentationStyle = UIModalPresentationFullScreen;
    UIViewController* root = win.rootViewController;
    while (root.presentedViewController) root = root.presentedViewController;
    [root presentViewController:c animated:NO completion:nil];
}

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
        /* thermal: 0 nominal, 1 fair, 2 serious, 3 critical (the OS slows the CPU from 2 up) */
        fprintf(stderr, "[ios-mem] available %zu MB, thermal %ld\n", os_proc_available_memory() >> 20,
                (long)NSProcessInfo.processInfo.thermalState);
        sleep((unsigned)secs);
    }
    return NULL;
}

/* IOS_CPU_LOG_SECS=<n> (default 0 = off): every n s, CPU use per thread over the last n s
 * (100 = one core busy), busiest first, with the thread's name and QoS. Xcode's Time Profiler
 * cannot record over Wi-Fi; this shows which thread limits the frame rate without it. */
static const char* qos_name(qos_class_t q)
{
    switch (q) {
    case QOS_CLASS_USER_INTERACTIVE: return "UI";
    case QOS_CLASS_USER_INITIATED:   return "IN";
    case QOS_CLASS_DEFAULT:          return "DF";
    case QOS_CLASS_UTILITY:          return "UT";
    case QOS_CLASS_BACKGROUND:       return "BG";
    default:                         return "--";
    }
}

static void* cpu_logger(void*)
{
    pthread_setname_np("ios-cpu");
    const char* e = getenv("IOS_CPU_LOG_SECS");
    const int secs = (e && *e) ? atoi(e) : 0;
    if (secs < 1) return NULL;
    enum { MAXT = 512 };
    static uint64_t prev_id[MAXT], prev_us[MAXT];
    static int nprev;
    for (;;) {
        sleep((unsigned)secs);
        thread_act_array_t th; mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) continue;
        struct Row { int pct; char name[40]; const char* qos; } rows[MAXT];
        static uint64_t cur_id[MAXT], cur_us[MAXT];
        int nr = 0, ncur = 0, total = 0;
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
            thread_identifier_info_data_t ii; mach_msg_type_number_t c2 = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
                thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &c2) == KERN_SUCCESS &&
                ncur < MAXT) {
                const uint64_t us = (uint64_t)bi.user_time.seconds * 1000000 + bi.user_time.microseconds +
                                    (uint64_t)bi.system_time.seconds * 1000000 + bi.system_time.microseconds;
                uint64_t was = us;   /* new thread: counts from the next round */
                for (int k = 0; k < nprev; k++) if (prev_id[k] == ii.thread_id) { was = prev_us[k]; break; }
                cur_id[ncur] = ii.thread_id; cur_us[ncur] = us; ncur++;
                const int pct = (int)((us - was) / (10000ull * (uint64_t)secs));
                total += pct;
                if (pct >= 3) {
                    Row& r = rows[nr++];
                    r.pct = pct; r.name[0] = 0; r.qos = "--";
                    pthread_t pt = pthread_from_mach_thread_np(th[i]);
                    if (pt) {
                        pthread_getname_np(pt, r.name, sizeof r.name);
                        qos_class_t q; int rel;
                        if (pthread_get_qos_class_np(pt, &q, &rel) == 0) r.qos = qos_name(q);
                    }
                    if (!r.name[0]) snprintf(r.name, sizeof r.name, "tid%llu", (unsigned long long)ii.thread_id);
                }
            }
            mach_port_deallocate(mach_task_self(), th[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
        memcpy(prev_id, cur_id, sizeof(uint64_t) * ncur); memcpy(prev_us, cur_us, sizeof(uint64_t) * ncur);
        nprev = ncur;
        for (int a = 1; a < nr; a++)   /* busiest first */
            for (int b = a; b > 0 && rows[b].pct > rows[b - 1].pct; b--) { Row t = rows[b]; rows[b] = rows[b - 1]; rows[b - 1] = t; }
        char line[2048]; int o = snprintf(line, sizeof line, "[ios-cpu] total %d%%, %u threads:", total, (unsigned)n);
        for (int a = 0; a < nr && o < (int)sizeof line - 64; a++)
            o += snprintf(line + o, sizeof line - o, " %s %d (%s)%s", rows[a].name, rows[a].pct, rows[a].qos, a + 1 < nr ? "," : "");
        fprintf(stderr, "%s\n", line);
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

/* The desktop build runs the guest on the process main thread (8 MB
 * on macOS, more on Windows); iOS gives other threads 512 KB. */
static void start_game_thread(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t gt;
    if (pthread_create(&gt, &attr, game_thread, NULL) != 0) {
        fprintf(stderr, "[ios] could not start the game thread\n");
        exit(1);
    }
    pthread_attr_destroy(&attr);
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

        /* INFAMOUS_DATA_DIR (testing): keep the game folder there instead of
         * in Documents; absolute, or relative to Documents. main.cpp
         * (find_default_elf) reads the absolute path back. */
        NSString* data = docs;
        if (const char* dd = getenv("INFAMOUS_DATA_DIR"); dd && *dd) {
            NSString* d = [NSString stringWithUTF8String:dd];
            data = d.isAbsolutePath ? d : [docs stringByAppendingPathComponent:d];
            [NSFileManager.defaultManager createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
            setenv("INFAMOUS_DATA_DIR", data.fileSystemRepresentation, 1);
            fprintf(stderr, "[ios] data folder: %s\n", data.fileSystemRepresentation);
        }

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
        const bool cpu_ok = cpu_supported();
        if (uiwin && cpu_ok) {
            port_touch_pad_attach(uiwin);
            update_touch_pad();
            s_uiwin = (__bridge UIWindow*)uiwin;
            s_layer = layer;
            log_geometry("start");
        }

        pthread_t mt;
        pthread_create(&mt, NULL, memory_logger, NULL);
        pthread_detach(mt);
        pthread_t ct;
        pthread_create(&ct, NULL, cpu_logger, NULL);
        pthread_detach(ct);

        s_game_argc = argc;
        s_game_argv = argv;
        /* No game path given (a normal launch) and no finished import in
         * Documents: ask for the player's copy first (ios_import.mm). */
        if (!cpu_ok) {
            fprintf(stderr, "[ios] this device is too old (needs A14 / M1 or newer): not starting the game\n");
            if (uiwin) {
                UIWindow* w = (__bridge UIWindow*)uiwin;
                dispatch_async(dispatch_get_main_queue(), ^{ show_too_old(w); });
            }
        } else if (argc < 2 && !di_installed(data.fileSystemRepresentation, NULL, 0) && uiwin) {
            fprintf(stderr, "[ios] no game in %s: showing the import screen\n", data.fileSystemRepresentation);
            std::string dest = data.fileSystemRepresentation;
            dispatch_async(dispatch_get_main_queue(), ^{
                port_ios_import_show(uiwin, dest.c_str(), ^{
                    fprintf(stderr, "[ios] import done: starting the game\n");
                    start_game_thread();
                });
            });
        } else {
            start_game_thread();
        }
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
