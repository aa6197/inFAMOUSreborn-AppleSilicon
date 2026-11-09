/* macOS app (Apple port): the .app bundle set-up and the first-run import.
 *
 * port_mac_app_init: only inside a .app bundle (a release build started from
 * Finder). Uses ~/Library/Application Support/inFamous as the working folder
 * (settings, saves, logs, the imported game), sends the logs there when there
 * is no terminal, and reads Contents/Resources/launch.env, the launcher
 * environment from installer/infamous.toml (values already set win).
 *
 * port_mac_find_game: called by main.cpp when no game path was given and none
 * was found next to the program. Returns the imported game in the working
 * folder, or shows a window that asks for the player's disc image or game
 * folder and imports it there (import/disc_import.c). NULL if the player quits.
 *
 * The plain build-mac binary run from a terminal with a game path never gets
 * here, so the developer tooling is unchanged. */
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../import/disc_import.h"

/* INFAMOUS_DATA_DIR overrides the folder (tests, or a game kept on another drive). */
static NSString* data_dir(void)
{
    const char* e = getenv("INFAMOUS_DATA_DIR");
    NSString* base = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString* d = (e && *e) ? [NSString stringWithUTF8String:e] : [base stringByAppendingPathComponent:@"inFamous"];
    [NSFileManager.defaultManager createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d;
}

static void start_app(void);

static bool in_app_bundle(void)
{
    return [NSBundle.mainBundle.bundlePath.pathExtension isEqualToString:@"app"];
}

extern "C" void port_mac_app_init(void)
{
    @autoreleasepool {
        if (!in_app_bundle()) return;
        NSString* d = data_dir();
        chdir(d.fileSystemRepresentation);
        /* Started from Finder: stdout/stderr are /dev/null. Keep a terminal
         * or a redirect as it is. */
        struct stat st;
        if (fstat(STDERR_FILENO, &st) == 0 && S_ISCHR(st.st_mode) && !isatty(STDERR_FILENO)) {
            freopen([d stringByAppendingPathComponent:@"play.out.log"].fileSystemRepresentation, "w", stdout);
            freopen([d stringByAppendingPathComponent:@"play.err.log"].fileSystemRepresentation, "w", stderr);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
        NSString* envp = [NSBundle.mainBundle pathForResource:@"launch" ofType:@"env"];
        FILE* f = envp ? fopen(envp.fileSystemRepresentation, "r") : NULL;
        int n = 0;
        if (f) {
            char line[4096];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                char* eq = strchr(line, '=');
                if (!eq || eq == line) continue;
                *eq = 0;
                setenv(line, eq + 1, 0);
                n++;
            }
            fclose(f);
        }
        fprintf(stderr, "[mac] app bundle: folder %s, launch.env %d variables\n", d.fileSystemRepresentation, n);
        start_app();
    }
}

/* ------------------------------------------------------------------------- */

@interface PortMacImport : NSObject <NSWindowDelegate>
@property(nonatomic, copy) NSString* destRoot;
@property(nonatomic, readonly) NSWindow* window;
- (void)show;
- (void)importFrom:(NSString*)path;
@end

@implementation PortMacImport {
    NSWindow*            _win;
    NSTextField*         _body;
    NSTextField*         _status;
    NSTextField*         _error;
    NSButton*            _pick;
    NSButton*            _quit;
    NSButton*            _stop;
    NSProgressIndicator* _bar;
    std::string          _source;
    std::string          _key;
    BOOL                 _pickingKey;
    BOOL                 _busy;
    volatile int         _cancel;
}

static NSTextField* label(NSString* s, CGFloat size, NSFontWeight w, NSColor* c)
{
    NSTextField* t = [NSTextField wrappingLabelWithString:s ?: @""];
    t.font = [NSFont systemFontOfSize:size weight:w];
    t.textColor = c;
    t.alignment = NSTextAlignmentCenter;
    t.translatesAutoresizingMaskIntoConstraints = NO;
    t.selectable = NO;
    return t;
}

- (NSWindow*)window { return _win; }

/* Test hook: INFAMOUS_UI_SNAPSHOT=<dir> writes the window as <dir>/ui-<tag>.png
 * (the window draws itself; no screen recording needed). */
- (void)snapshot:(NSString*)tag
{
    const char* dir = getenv("INFAMOUS_UI_SNAPSHOT");
    if (!dir || !*dir) return;
    NSView* v = _win.contentView.superview ?: _win.contentView;   /* the frame view draws the background */
    [v layoutSubtreeIfNeeded];
    [v displayIfNeeded];
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString* f = [[NSString stringWithUTF8String:dir] stringByAppendingPathComponent:
                   [NSString stringWithFormat:@"ui-%@.png", tag]];
    [png writeToFile:f atomically:YES];
}

/* Start an import without the open panel (the test hook INFAMOUS_IMPORT_TEST). */
- (void)importFrom:(NSString*)path
{
    _source = path.fileSystemRepresentation;
    _key.clear();
    [self run];
}

- (void)show
{
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 420)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                         backing:NSBackingStoreBuffered defer:NO];
    _win.title = @"inFamous";
    _win.delegate = self;
    _win.releasedWhenClosed = NO;
    _win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

    NSTextField* title = label(@"inFamous", 30, NSFontWeightBold, NSColor.labelColor);
    _body = label(@"To play, choose your copy of the game:\n"
                   "a disc image (.iso), or a game folder (the one with PS3_GAME inside).\n\n"
                   "It needs the European disc, BCES00220. The app copies what it needs "
                   "(about 7.5 GB) into your Library folder and does not change your file.",
                  14, NSFontWeightRegular, NSColor.secondaryLabelColor);
    _status = label(@"", 13, NSFontWeightRegular, NSColor.secondaryLabelColor);
    _error  = label(@"", 14, NSFontWeightSemibold, NSColor.systemRedColor);
    _pick = [NSButton buttonWithTitle:@"Choose disc image or folder…" target:self action:@selector(pick:)];
    _pick.keyEquivalent = @"\r";
    _pick.controlSize = NSControlSizeLarge;
    _quit = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)];
    _quit.controlSize = NSControlSizeLarge;
    _stop = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stop:)];
    _stop.controlSize = NSControlSizeLarge;
    _stop.hidden = YES;
    _bar = [NSProgressIndicator new];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    _bar.hidden = YES;
    _bar.translatesAutoresizingMaskIntoConstraints = NO;

    NSStackView* buttons = [NSStackView new];
    [buttons setViews:@[ _quit, _pick, _stop ] inGravity:NSStackViewGravityCenter];   /* centred row */
    buttons.spacing = 12;
    NSStackView* st = [NSStackView stackViewWithViews:@[ title, _body, _bar, _status, _error, buttons ]];
    st.orientation = NSUserInterfaceLayoutOrientationVertical;
    st.alignment = NSLayoutAttributeCenterX;
    st.spacing = 16;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* cv = _win.contentView;
    [cv addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [st.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:32],
        [st.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-32],
        [st.centerYAnchor constraintEqualToAnchor:cv.centerYAnchor],
        [st.topAnchor constraintGreaterThanOrEqualToAnchor:cv.topAnchor constant:28],
        [st.bottomAnchor constraintLessThanOrEqualToAnchor:cv.bottomAnchor constant:-28],
        [buttons.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_body.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_status.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_error.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_bar.widthAnchor constraintEqualToAnchor:st.widthAnchor],
    ]];
    [_win center];
    [_win makeKeyAndOrderFront:nil];
    [self snapshot:@"start"];
}

- (void)setBusy:(BOOL)busy
{
    _busy = busy;
    _pick.hidden = busy;
    _quit.hidden = busy;
    _stop.hidden = !busy;
    _bar.hidden = !busy;
    if (busy) { _error.stringValue = @""; _bar.doubleValue = 0; }
}

- (void)showError:(NSString*)msg
{
    [self setBusy:NO];
    _status.stringValue = @"";
    _error.stringValue = msg ?: @"";
    _pick.title = _pickingKey ? @"Choose disc key (.dkey)…" : @"Choose disc image or folder…";
}

- (void)pick:(id)sender
{
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.allowsMultipleSelection = NO;
    if (_pickingKey) {
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.message = @"Choose the disc key file (.dkey) for this disc image.";
    } else {
        p.canChooseFiles = YES;
        p.canChooseDirectories = YES;
        p.message = @"Choose the inFamous disc image (.iso), or the game folder with PS3_GAME inside.";
    }
    p.prompt = @"Choose";
    [p beginSheetModalForWindow:_win completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !p.URL) return;
        if (self->_pickingKey) {
            self->_key = p.URL.fileSystemRepresentation;
        } else {
            self->_source = p.URL.fileSystemRepresentation;
            self->_key.clear();
        }
        [self run];
    }];
}

- (void)run
{
    const std::string src = _source, key = _key, dest = self.destRoot.fileSystemRepresentation;
    fprintf(stderr, "[import] source %s%s%s\n", src.c_str(), key.empty() ? "" : ", key ", key.c_str());
    [self setBusy:YES];
    _status.stringValue = @"Checking…";
    _cancel = 0;
    id activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityIdleSystemSleepDisabled
                                                               reason:@"Importing the game"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char err[1024] = "";
        di_info info;
        di_status s = di_probe(src.c_str(), key.empty() ? NULL : key.c_str(), &info, err, sizeof err);
        if (s == DI_OK) {
            di_callbacks cb;
            cb.progress = [](void* ctx, double f, const char* what) {
                PortMacImport* me = (__bridge PortMacImport*)ctx;
                NSString* w = [NSString stringWithUTF8String:what ?: ""];
                dispatch_async(dispatch_get_main_queue(), ^{ [me progress:f status:w]; });
            };
            cb.ctx = (__bridge void*)self;
            cb.cancel = &self->_cancel;
            s = di_import(src.c_str(), key.empty() ? NULL : key.c_str(), dest.c_str(), &cb, err, sizeof err);
        }
        fprintf(stderr, "[import] %s%s%s\n", di_status_name(s), err[0] ? ": " : "", err);
        NSString* msg = [NSString stringWithUTF8String:err];
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSProcessInfo.processInfo endActivity:activity];
            [self finished:s message:msg];
        });
    });
}

- (void)progress:(double)f status:(NSString*)what
{
    _bar.doubleValue = f;
    _status.stringValue = [NSString stringWithFormat:@"%d%%  %@", (int)(f * 100), what];
    static bool shot;
    if (!shot && f >= 0.3) { shot = true; [self snapshot:@"progress"]; }
}

- (void)finished:(di_status)s message:(NSString*)msg
{
    if (s == DI_ERR_NEED_KEY || s == DI_ERR_BAD_KEY) {
        _pickingKey = YES;
        [self showError:msg];
        [self snapshot:@"needkey"];
        return;
    }
    _pickingKey = NO;
    if (s != DI_OK) { [self showError:msg]; [self snapshot:@"error"]; return; }
    [self setBusy:NO];
    _pick.hidden = YES;
    _quit.hidden = YES;
    _status.stringValue = @"Done. Starting the game…";
    [self snapshot:@"done"];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self->_win orderOut:nil];
        [NSApp stopModalWithCode:NSModalResponseOK];
    });
}

- (void)stop:(id)sender
{
    _cancel = 1;
    _status.stringValue = @"Stopping…";
}

- (void)quit:(id)sender
{
    [_win orderOut:nil];
    [NSApp stopModalWithCode:NSModalResponseCancel];
}

- (BOOL)windowShouldClose:(NSWindow*)w
{
    if (_busy) { [self stop:nil]; return NO; }
    [self quit:nil];
    return NO;
}

@end

/* The app menu (the Metal backend's window comes without one). */
static void build_menu(void)
{
    if (NSApp.mainMenu) return;
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"inFamous"];
    [app addItemWithTitle:@"Hide inFamous" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* others = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quit inFamous" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = app;
    NSMenuItem* winItem = [NSMenuItem new];
    [bar addItem:winItem];
    NSMenu* wm = [[NSMenu alloc] initWithTitle:@"Window"];
    [wm addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [wm addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    NSMenuItem* fs = [wm addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    winItem.submenu = wm;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = wm;
}

/* NSApp as the Metal backend makes it (rsx_metal_backend.m create_window
 * uses the same shared NSApplication), plus the menu. Main thread. */
static void start_app(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    build_menu();
    [NSApp finishLaunching];
}

/* With a window, the Metal backend creates it and pumps its events on the
 * main thread (dispatch_sync to the main queue from the RSX thread). So the
 * main thread must run the AppKit loop, and the game runs on a thread of its
 * own (as on iOS, ios_shell.mm). Headless runs keep the game on the main
 * thread, as before. */
struct PortMacGame { int (*fn)(void*); void* arg; };

static void* port_mac_game_thread(void* p)
{
    pthread_setname_np("game-main");
    PortMacGame g = *(PortMacGame*)p;
    exit(g.fn(g.arg));   /* what returning from main() would do */
}

extern "C" int port_mac_run_game(int (*fn)(void*), void* arg)
{
    const char* hl = getenv("PS3RECOMP_METAL_HEADLESS");
    if (hl && *hl && *hl != '0') return fn(arg);
    static PortMacGame g;
    g = { fn, arg };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t t;
    const int err = pthread_create(&t, &attr, port_mac_game_thread, &g);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "[mac] could not start the game thread (%d)\n", err);
        return 1;
    }
    @autoreleasepool {
        start_app();
    }
    [NSApp run];   /* never returns: the game thread, Quit or closing the window ends the process */
    return 0;
}

extern "C" const char* port_mac_find_game(char* out, size_t len)
{
    @autoreleasepool {
        NSString* d = data_dir();
        if (di_installed(d.fileSystemRepresentation, out, len)) {
            fprintf(stderr, "[mac] game: %s\n", out);
            return out;
        }
        fprintf(stderr, "[mac] no game in %s: showing the import window\n", d.fileSystemRepresentation);
        start_app();
        if (@available(macOS 14.0, *)) [NSApp activate];
        else                           [NSApp activateIgnoringOtherApps:YES];

        PortMacImport* ui = [PortMacImport new];
        ui.destRoot = d;
        [ui show];
        if (const char* t = getenv("INFAMOUS_IMPORT_TEST"); t && *t) {
            NSString* src = [NSString stringWithUTF8String:t];
            dispatch_async(dispatch_get_main_queue(), ^{ [ui importFrom:src]; });
        }
        const NSModalResponse r = [NSApp runModalForWindow:ui.window];
        if (r != NSModalResponseOK) {
            fprintf(stderr, "[mac] import window closed\n");
            return NULL;
        }
        if (!di_installed(d.fileSystemRepresentation, out, len)) return NULL;
        fprintf(stderr, "[mac] game: %s\n", out);
        return out;
    }
}
