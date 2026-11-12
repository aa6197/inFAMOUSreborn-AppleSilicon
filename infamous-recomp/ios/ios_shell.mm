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
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <unistd.h>
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

#include "../apple/gfx_settings.h"

extern "C" int  port_game_main(int argc, char** argv);   /* main.cpp */
extern "C" void port_app_set_paused(int paused);         /* main.cpp */
extern "C" int  port_app_rsx_parked(void);               /* main.cpp */
extern "C" void rsx_metal_backend_set_layer(void* ca_metal_layer);
extern "C" void rsx_metal_backend_wait_idle(void);
extern "C" void port_touch_pad_attach(void* uiwindow);   /* touch_pad.mm */
extern "C" void port_touch_pad_gamepads(int count);     /* touch_pad.mm, main thread */
extern "C" void port_ios_import_show(void* uiwindow, const char* dest_root, void (^done)(void));   /* ios_import.mm */
extern "C" int  di_installed(const char* dest_root, char* elf_out, size_t elf_out_len);              /* import/disc_import.c */
extern "C" volatile int g_ab_phase;                     /* ppu_loader.cpp (PS3_AB in-run A/B) */

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
        fprintf(stderr, "[ios-mem] available %zu MB, thermal %ld, low power mode %d\n", os_proc_available_memory() >> 20,
                (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
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

/* Same interval, whole process (rusage v6): how the CPU time splits between the P cores and the
 * E cores, their clock and IPC, and how many threads on average were ready but waiting for a core
 * (runnable time counts running + waiting). libproc.h is not in the iOS SDK; the call is. */
extern "C" int proc_pid_rusage(int pid, int flavor, rusage_info_t* buffer);
static void log_core_split(unsigned secs)
{
    static struct rusage_info_v6 prev;
    static int have_prev;
    struct rusage_info_v6 r;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&r) != 0) return;
    if (have_prev) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const double ns = (double)tb.numer / tb.denom;   /* rusage times are mach ticks */
        const double all = ((r.ri_user_time + r.ri_system_time) - (prev.ri_user_time + prev.ri_system_time)) * ns;
        const double p = ((r.ri_user_ptime + r.ri_system_ptime) - (prev.ri_user_ptime + prev.ri_system_ptime)) * ns;
        const double e = all - p;
        const double pcyc = (double)(r.ri_pcycles - prev.ri_pcycles);
        const double ecyc = (double)((r.ri_cycles - r.ri_pcycles) - (prev.ri_cycles - prev.ri_pcycles));
        const double pins = (double)(r.ri_pinstructions - prev.ri_pinstructions);
        const double eins = (double)((r.ri_instructions - r.ri_pinstructions) - (prev.ri_instructions - prev.ri_pinstructions));
        const double run = (r.ri_runnable_time - prev.ri_runnable_time) * ns;
        const double wall = secs * 1e9;
        fprintf(stderr, "[ios-cpu] P cores %.0f%% / E cores %.0f%% (100 = one core), P %.2f GHz IPC %.2f, "
                        "E %.2f GHz IPC %.2f, waiting for a core %.2f threads\n",
                100.0 * p / wall, 100.0 * e / wall,
                p > 0 ? pcyc / p : 0.0, pcyc > 0 ? pins / pcyc : 0.0,
                e > 0 ? ecyc / e : 0.0, ecyc > 0 ? eins / ecyc : 0.0,
                (run - all) / wall);
    }
    prev = r; have_prev = 1;
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
        log_core_split((unsigned)secs);
    }
    return NULL;
}

/* IOS_RUNSTATE=1 (default off): every 1 ms, read the run state of the main game thread and the RSX
 * threads. Mach reports TH_STATE_RUNNING for running AND ready-to-run, so "ready" minus the CPU
 * time the thread got is how long it was ready but waiting for a core. One line per thread every
 * 10 s: on CPU / ready, waiting for a core / blocked. Shows which side limits when cores are full. */
static void* runstate_sampler(void*)
{
    pthread_setname_np("ios-runstate");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const char* e = getenv("IOS_RUNSTATE");
    if (!e || *e != '1') return NULL;
    static const char* const want[] = { "game-main", "rsx fifo", "rsx_encode" };
    enum { NW = 3 };
    for (;;) {
        thread_act_t port[NW] = { MACH_PORT_NULL, MACH_PORT_NULL, MACH_PORT_NULL };
        thread_act_array_t th; mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) { sleep(10); continue; }
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            int keep = 0;
            pthread_t pt = pthread_from_mach_thread_np(th[i]);
            char nm[40] = "";
            if (pt) pthread_getname_np(pt, nm, sizeof nm);
            for (int k = 0; k < NW; k++)
                if (port[k] == MACH_PORT_NULL && strcmp(nm, want[k]) == 0) { port[k] = th[i]; keep = 1; break; }
            if (!keep) mach_port_deallocate(mach_task_self(), th[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
        uint32_t ready[NW] = { 0 }, samples = 0;
        uint64_t us0[NW] = { 0 }, us1[NW] = { 0 };
        auto cpu_us = [](thread_act_t t) -> uint64_t {
            thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS) return 0;
            return (uint64_t)bi.user_time.seconds * 1000000 + bi.user_time.microseconds +
                   (uint64_t)bi.system_time.seconds * 1000000 + bi.system_time.microseconds;
        };
        for (int k = 0; k < NW; k++) if (port[k] != MACH_PORT_NULL) us0[k] = cpu_us(port[k]);
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0 < 10000000000ull) {
            for (int k = 0; k < NW; k++) {
                if (port[k] == MACH_PORT_NULL) continue;
                thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
                if (thread_info(port[k], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
                    bi.run_state == TH_STATE_RUNNING) ready[k]++;
            }
            samples++;
            usleep(1000);
        }
        const double wall_us = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000.0;
        char line[512]; int o = snprintf(line, sizeof line, "[ios-runstate] %u samples:", samples);
        for (int k = 0; k < NW; k++) {
            if (port[k] == MACH_PORT_NULL) continue;
            us1[k] = cpu_us(port[k]);
            const double oncpu = 100.0 * (double)(us1[k] - us0[k]) / wall_us;
            const double rdy = samples ? 100.0 * ready[k] / samples : 0.0;
            o += snprintf(line + o, sizeof line - (size_t)o, " %s on CPU %.0f%%, ready %.0f%% (waiting for a core %.0f%%), blocked %.0f%% |",
                          want[k], oncpu, rdy, rdy > oncpu ? rdy - oncpu : 0.0, 100.0 - rdy);
            mach_port_deallocate(mach_task_self(), port[k]);
        }
        fprintf(stderr, "%s\n", line);
    }
    return NULL;
}

/* IOS_FREQ_PROBE=<n> (default 0 = off): every n s, one user-interactive thread runs 10^9 dependent
 * adds (one cycle each) and logs the clock it got, by its own CPU time. Shows whether a busy thread
 * can still get a high clock while the game runs, or whether the cores are held low. */
static void* freq_probe(void*)
{
    pthread_setname_np("ios-freq");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const char* e = getenv("IOS_FREQ_PROBE");
    const int secs = (e && *e) ? atoi(e) : 0;
    if (secs < 1) return NULL;
    for (;;) {
        sleep((unsigned)secs);
        const uint64_t n = 1000000000ull;
        uint64_t x = 0;
        const uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (uint64_t i = 0; i < n; i += 8) {
#if defined(__aarch64__)
            __asm__ volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                             "add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1" : "+r"(x));
#endif
        }
        const uint64_t c1 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), w1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        fprintf(stderr, "[ios-freq] busy thread got %.2f GHz (on its CPU time), ran %.0f%% of the wall time\n",
                (double)n / (double)(c1 - c0), 100.0 * (double)(c1 - c0) / (double)(w1 - w0));
    }
    return NULL;
}

/* IOS_CPU_STRESS=<secs> (default 0 = off): before the game starts, run 1, 2, ... (one per core)
 * busy user-interactive threads for <secs> each and log the clock each step got, with the P/E split.
 * Shows the clock the device gives a given number of busy cores without the game's GPU work. */
struct StressArg { volatile int* stop; uint64_t adds; uint64_t cpu_ns; };
static void* stress_worker(void* a)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    StressArg* sa = (StressArg*)a;
    const uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    uint64_t x = 0;
    /* IOS_CPU_STRESS_ILP=1: six independent integer adds and four independent vector FMAs per step,
     * so the core runs near its full width and draws game-like power (the game runs at IPC ~4.6 on
     * the P cores). The per-thread GHz figure is then meaningless; read the P/E GHz from rusage. */
    static const int ilp = [] { const char* v = getenv("IOS_CPU_STRESS_ILP"); return v && v[0] == '1'; }();
    while (!__atomic_load_n(sa->stop, __ATOMIC_RELAXED)) {
        for (int i = 0; i < 10000000; i += 8) {
#if defined(__aarch64__)
            if (ilp)
                __asm__ volatile("add x9,x9,#1\n add x10,x10,#1\n add x11,x11,#1\n"
                                 "add x12,x12,#1\n add x13,x13,#1\n add x14,x14,#1\n"
                                 "fmla v16.4s,v20.4s,v21.4s\n fmla v17.4s,v20.4s,v21.4s\n"
                                 "fmla v18.4s,v20.4s,v21.4s\n fmla v19.4s,v20.4s,v21.4s"
                                 ::: "x9", "x10", "x11", "x12", "x13", "x14",
                                     "v16", "v17", "v18", "v19");
            else
            __asm__ volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                             "add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1" : "+r"(x));
#endif
        }
        sa->adds += 10000000;
    }
    sa->cpu_ns = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0;
    return NULL;
}

/* IOS_GPU_STRESS=<iters> (with IOS_CPU_STRESS): keep the GPU busy during the CPU steps with a compute
 * kernel (<iters> multiply-adds per GPU thread, 256K threads per command buffer, the CPU waits for each
 * one), plus IOS_GPU_STRESS_SLEEP_MS=<ms> between buffers. Does GPU use alone lower the CPU clock? */
static volatile int s_gpu_stress_stop;
static void* gpu_stress(void*)
{
    pthread_setname_np("ios-gpu-stress");
    const int iters = atoi(getenv("IOS_GPU_STRESS"));
    const char* sl = getenv("IOS_GPU_STRESS_SLEEP_MS");
    const int sleep_ms = (sl && *sl) ? atoi(sl) : 0;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSString* src = [NSString stringWithFormat:@
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void spin(device float* o [[buffer(0)]], uint id [[thread_position_in_grid]]) {"
        " float x = float(id) * 1e-6f; for (int i = 0; i < %d; i++) x = fma(x, 1.0001f, 0.5f) * 0.9999f; o[id] = x; }", iters];
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    id<MTLComputePipelineState> ps = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&err] : nil;
    if (!ps) { fprintf(stderr, "[ios-stress] GPU kernel failed: %s\n", err.localizedDescription.UTF8String); return NULL; }
    const NSUInteger n = 256 * 1024;
    id<MTLBuffer> out = [dev newBufferWithLength:n * sizeof(float) options:MTLResourceStorageModePrivate];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    uint64_t busy = 0, t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), cbs = 0;
    while (!s_gpu_stress_stop) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            busy += (uint64_t)((cb.GPUEndTime - cb.GPUStartTime) * 1e9); cbs++;
        }
        if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000);
    }
    const double wall = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    fprintf(stderr, "[ios-stress] GPU busy %.0f%% of the time, %.2f ms per buffer\n", 100.0 * busy / wall,
            cbs ? busy / 1e6 / cbs : 0.0);
    return NULL;
}

static void cpu_stress_ladder(void)
{
    const char* e = getenv("IOS_CPU_STRESS");
    const int secs = (e && *e) ? atoi(e) : 0;
    if (secs < 1) return;
    pthread_t gt = 0;
    if (const char* g = getenv("IOS_GPU_STRESS"); g && atoi(g) > 0) pthread_create(&gt, NULL, gpu_stress, NULL);
    const int ncpu = (int)NSProcessInfo.processInfo.activeProcessorCount;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const double tick_ns = (double)tb.numer / tb.denom;
    /* IOS_CPU_STRESS_N=<n> IOS_CPU_STRESS_REPEAT=<r>: only n threads, r steps in a row (does the clock
     * hold over minutes?). */
    const char* en = getenv("IOS_CPU_STRESS_N");
    const char* er = getenv("IOS_CPU_STRESS_REPEAT");
    const int fixed_n = (en && *en) ? atoi(en) : 0;
    const int steps = fixed_n > 0 ? ((er && *er) ? atoi(er) : 1) : ncpu;
    for (int step = 1; step <= steps; step++) {
        const int n = fixed_n > 0 ? fixed_n : step;
        enum { MAXW = 32 };
        pthread_t th[MAXW]; StressArg sa[MAXW]; volatile int stop = 0;
        struct rusage_info_v6 r0, r1;
        proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&r0);
        const uint64_t w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (int i = 0; i < n && i < MAXW; i++) {
            sa[i] = StressArg{ &stop, 0, 0 };
            pthread_create(&th[i], NULL, stress_worker, &sa[i]);
        }
        sleep((unsigned)secs);
        __atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < n && i < MAXW; i++) pthread_join(th[i], NULL);
        const double wall = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0);
        proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&r1);
        double lo = 1e9, hi = 0, sum = 0;
        for (int i = 0; i < n && i < MAXW; i++) {
            const double g = sa[i].cpu_ns ? (double)sa[i].adds / (double)sa[i].cpu_ns : 0;
            lo = g < lo ? g : lo; hi = g > hi ? g : hi; sum += g;
        }
        const double all = ((r1.ri_user_time + r1.ri_system_time) - (r0.ri_user_time + r0.ri_system_time)) * tick_ns;
        const double p = ((r1.ri_user_ptime + r1.ri_system_ptime) - (r0.ri_user_ptime + r0.ri_system_ptime)) * tick_ns;
        const double pc = (double)(r1.ri_pcycles - r0.ri_pcycles);
        const double ec = (double)((r1.ri_cycles - r1.ri_pcycles) - (r0.ri_cycles - r0.ri_pcycles));
        fprintf(stderr, "[ios-stress] %d busy threads: %.2f GHz each (min %.2f, max %.2f); P cores %.0f%% at %.2f GHz, "
                        "E cores %.0f%% at %.2f GHz\n", n, sum / n, lo, hi,
                100.0 * p / wall, p > 0 ? pc / p : 0.0, 100.0 * (all - p) / wall, all - p > 0 ? ec / (all - p) : 0.0);
    }
    if (gt) { s_gpu_stress_stop = 1; pthread_join(gt, NULL); }
}

static void* game_thread(void*)
{
    pthread_setname_np("game-main");
    cpu_stress_ladder();
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
        port_gfx_apply_live();   /* a Graphics choice made in the Settings app meanwhile */
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
        fprintf(stderr, "[ios] low power mode %d, thermal %ld\n", (int)pi.lowPowerModeEnabled, (long)pi.thermalState);

        port_gfx_apply_env();   /* the Settings app's Graphics choices, before launch.env */
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
        pthread_t ft;
        pthread_create(&ft, NULL, freq_probe, NULL);
        pthread_detach(ft);
        pthread_t rt;
        pthread_create(&rt, NULL, runstate_sampler, NULL);
        pthread_detach(rt);

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

    /* IOS_PUMP_MS (default 4): SDL3 has no real event wait on UIKit. SDL_WaitEventTimeout
     * pumps the run loop and sleeps 1 ms, again and again: ~1000 wake-ups/s, ~12% of a core
     * on the iPhone (profile: SDL_PumpEventsInternal -> UIKit_PumpEvents). Now: take what SDL
     * has (one pump: gamepads, queued events), then sleep in the run loop for up to this many
     * ms. A touch, a dispatch to the main queue or any other run-loop source wakes it at once
     * (touch_pad.mm is a UIKit view, so the on-screen pad never waits on this). Gamepads are
     * sampled at 1000/<ms> Hz instead of 1000 Hz. 0 = the old SDL_WaitEventTimeout loop.
     * IOS_PUMP_PHASE=0|1: on only in PS3_AB phases of that parity (in-run A/B). */
    int pump_ms = 4, pump_phase = -1;
    if (const char* p = getenv("IOS_PUMP_MS"); p && *p) pump_ms = atoi(p) < 0 ? 0 : atoi(p) > 50 ? 50 : atoi(p);
    if (const char* p = getenv("IOS_PUMP_PHASE"); p && *p) pump_phase = atoi(p) & 1;
    fprintf(stderr, "[ios] main loop: %s (IOS_PUMP_MS=%d, phase %d)\n",
            pump_ms ? "run-loop wait" : "SDL_WaitEventTimeout (1 ms polling)", pump_ms, pump_phase);
    const Uint64 t0 = SDL_GetTicks();
    int geo_step = 0;
    for (;;) {
        @autoreleasepool {
            SDL_Event e;
            bool got;
            if (pump_ms && (pump_phase < 0 || (g_ab_phase & 1) == pump_phase)) {
                got = SDL_PollEvent(&e);
                if (!got) CFRunLoopRunInMode(kCFRunLoopDefaultMode, pump_ms / 1000.0, true);
            } else {
                got = SDL_WaitEventTimeout(&e, 100);
            }
            if (got) {
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
