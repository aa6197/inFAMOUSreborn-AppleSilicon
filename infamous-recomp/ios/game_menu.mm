/* In-game menu for the iOS / iPadOS app (Apple port).
 *
 * Opens with the menu button on the touch controls (touch_pad.mm), or the
 * Home button (PS / Xbox / Guide) or the touchpad click on a gamepad. The game
 * pauses while it is up (port_app_set_paused(2): the frame clock parks, GPU
 * memory is kept).
 *
 * Two groups:
 * - "Saves and resolution": what a restart applies. Resolution (the Settings
 *   app's RenderScale) takes effect at the next start. Save-state builds
 *   (PORT_SAVE_STATES, port_ss_available()) add "Save state" (saves to
 *   Documents/save.ss now; the game stays paused) and "Load state" (picks that
 *   save, or auto.ss, made when the app last went to the background,
 *   ios_shell.mm). A load only works at a fresh start.
 *   When Resolution differs from what the game runs with, or a save is picked,
 *   an orange "Restart now" button shows in the group's header. It leaves the
 *   load marker (port_ss_load_next) and closes the app. iOS does not let an
 *   app open itself again, so first it sets a notification for a moment
 *   later: one tap on it opens the game (and the load runs). Notifications are
 *   asked for the first time Restart now is used; without them the menu says
 *   to open the game from the Home Screen. MENU_NOTIFY=0 (tests) skips the
 *   notification.
 * - "Graphics": Shadows and Upscaling, which change at once.
 * Both use the keys of the Settings app (apple/gfx_settings.h), so they always
 * agree. On a short, wide screen (iPhone in landscape) the groups sit side by
 * side.
 *
 * Gamepad: up / down picks a row, left / right changes it, Cross on Resume,
 * or Circle / START / Home anywhere, closes it. It closes when that button is
 * let go, so the game never sees the press. Cross presses Restart now and
 * Save now, and picks (or un-picks) the save on the Load row.
 */
#include <SDL3/SDL.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" void port_app_set_paused(int paused);   /* main.cpp: 0 run, 1 background, 2 menu */
extern "C" void port_touch_pad_menu(int open);     /* touch_pad.mm */
extern "C" void port_ios_gfx_apply_live(void);     /* ios_shell.mm */
extern "C" int  rsx_engine_shadow_maps(void);
extern "C" void port_game_menu_close(void);
/* runtime/ppu/ppu_ss_probe.cpp (all Apple builds; 0 / no-ops without save states) */
extern "C" int  port_ss_available(void);
extern "C" int  port_ss_save_async(const char* path, void (*done)(int ok, void* user), void* user);
extern "C" int  port_ss_save_busy(void);
extern "C" int  port_ss_load_next(const char* name);

typedef struct { const char* title; NSString* key; int n; const char* names[5]; double values[5]; } MenuRow;
static const MenuRow kRows[] = {
    { "Shadows",    @"Shadows",     3, { "On", "Near only", "Off" },                     { 2, 1, 0 } },
    { "Resolution", @"RenderScale", 5, { "720p", "630p", "540p", "450p", "360p" },        { 100, 87.5, 75, 62.5, 50 } },
    { "Upscaling",  @"Upscaling",   2, { "MetalFX", "Simple" },                          { 1, 0 } },
};
enum { NROWS = sizeof kRows / sizeof kRows[0], ROW_SHADOWS = 0, ROW_RESOLUTION = 1, ROW_UPSCALING = 2 };
/* What the gamepad can point at, top to bottom. */
enum { F_RESTART, F_SAVE, F_LOAD, F_RESOLUTION, F_SHADOWS, F_UPSCALING, F_RESUME, NFOCUS };
static int focus_row(int f)
{
    return f == F_RESOLUTION ? ROW_RESOLUTION : f == F_SHADOWS ? ROW_SHADOWS : f == F_UPSCALING ? ROW_UPSCALING : -1;
}
static int row_focus(int row)
{
    return row == ROW_RESOLUTION ? F_RESOLUTION : row == ROW_SHADOWS ? F_SHADOWS : F_UPSCALING;
}

/* Save slots in Documents: 0 the menu save, 1 the background save (ios_shell.mm). */
static const char* const kSlotFile[2] = { "save.ss", "auto.ss" };
static const char* const kSlotName[2] = { "Save", "Auto" };
static NSString* slot_path(int slot)
{
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [docs stringByAppendingPathComponent:[NSString stringWithUTF8String:kSlotFile[slot]]];
}
/* nil when there is no such save */
static NSDate* slot_date(int slot)
{
    NSDictionary* a = [NSFileManager.defaultManager attributesOfItemAtPath:slot_path(slot) error:nil];
    return a ? a.fileModificationDate : nil;
}
static NSString* short_time(NSDate* d)
{
    NSDateFormatter* f = [[NSDateFormatter alloc] init];
    f.dateFormat = [NSCalendar.currentCalendar isDateInToday:d] ? @"HH:mm" : @"d MMM HH:mm";
    return [f stringFromDate:d];
}

static int index_of(const MenuRow* r, double v)
{
    int best = 0;
    for (int i = 1; i < r->n; i++)
        if (fabs(r->values[i] - v) < fabs(r->values[best] - v)) best = i;
    return best;
}

/* What the game runs with now, for a row the player never set. */
static double running_value(int row)
{
    const char* e;
    switch (row) {
    case ROW_SHADOWS: { const int n = rsx_engine_shadow_maps(); return n < 0 ? 2 : n == 0 ? 0 : 1; }
    case ROW_RESOLUTION: return (e = getenv("RENDER_SCALE")) && *e ? atof(e) : 100;
    default: return (e = getenv("METALFX")) && *e == '0' ? 0 : 1;
    }
}

@interface PortGameMenu : UIView
@property(nonatomic) int focus;
@property(nonatomic, readonly) BOOL restarting;
- (void)refresh;
- (void)move:(int)d;
- (void)change:(int)d;
- (void)activate;
- (void)saveDone:(int)ok;
- (void)saveTapped;
- (void)pickSlot:(int)slot;
- (void)pickResolution:(int)index;
- (void)restartTapped;
- (void)setHeadMessage:(NSString*)msg;
@end

static PortGameMenu* s_menu;
static NSString* const kReopenNote = @"port.reopen";
static NSString* const kOpenAgain = @"Closing. Open inFamous again from the Home Screen.";

/* save thread -> main thread */
static void menu_save_done(int ok, void*)
{
    dispatch_async(dispatch_get_main_queue(), ^{ [s_menu saveDone:ok]; });
}

/* Restart now, last step. Any thread. */
static void menu_exit_now(void)
{
    fprintf(stderr, "[ios] menu: closing the app\n");
    fflush(stderr);
    _exit(0);
}
/* No notification: say how to come back, give time to read it, close. Main thread. */
static void menu_exit_with_hint(void)
{
    [s_menu setHeadMessage:kOpenAgain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ menu_exit_now(); });
}
/* iOS does not let an app open itself again: a notification set for a moment after the
 * app has closed brings it back with one tap. Any thread. */
static void menu_notify_and_exit(NSString* body)
{
    UNMutableNotificationContent* n = [[UNMutableNotificationContent alloc] init];
    n.title = @"Tap to open inFamous again";
    n.body = body;
    UNNotificationRequest* r = [UNNotificationRequest requestWithIdentifier:kReopenNote content:n
        trigger:[UNTimeIntervalNotificationTrigger triggerWithTimeInterval:1 repeats:NO]];
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:r withCompletionHandler:^(NSError* err) {
        fprintf(stderr, "[ios] menu: reopen notification %s\n", err ? "FAILED" : "set");
        /* gone before it fires: iOS only shows it when the app is not on screen */
        if (!err) menu_exit_now();
        dispatch_async(dispatch_get_main_queue(), ^{ menu_exit_with_hint(); });
    }];
}
/* Asks for notifications the first time. Main thread. */
static void menu_reopen_then_exit(NSString* body)
{
    const char* e = getenv("MENU_NOTIFY");
    if (e && *e == '0') { menu_exit_with_hint(); return; }
    UNUserNotificationCenter* c = UNUserNotificationCenter.currentNotificationCenter;
    [c getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* s) {
        const UNAuthorizationStatus st = s.authorizationStatus;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (st == UNAuthorizationStatusDenied) {
                fprintf(stderr, "[ios] menu: notifications are off\n");
                menu_exit_with_hint();
            } else if (st == UNAuthorizationStatusNotDetermined) {
                fprintf(stderr, "[ios] menu: asking for notifications\n");
                [s_menu setHeadMessage:@"Allow notifications: then one tap opens the game again"];
                [c requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                                 completionHandler:^(BOOL granted, NSError*) {
                    fprintf(stderr, "[ios] menu: notifications %s\n", granted ? "allowed" : "not allowed");
                    if (granted) menu_notify_and_exit(body);
                    else dispatch_async(dispatch_get_main_queue(), ^{ menu_exit_with_hint(); });
                }];
            } else
                menu_notify_and_exit(body);
        });
    }];
}

@implementation PortGameMenu {
    UIView*             _panel;
    UILabel*            _title;
    UIView*             _box[2];      /* 0 saves and resolution, 1 graphics */
    UILabel*            _head[2];
    UIButton*           _restart;
    UILabel*            _label[NROWS];
    UISegmentedControl* _seg[NROWS];
    UIButton*           _resume;
    double              _startScale;
    BOOL                _ss;          /* save-state build: Save / Load rows */
    UILabel*            _ssLabel[2];
    UIButton*           _saveBtn;
    UIButton*           _loadBtn[2];
    int                 _loadSel;     /* slot the Load row points at (gamepad) */
    int                 _loadPick;    /* slot the restart loads, or -1 */
    BOOL                _wasPending;
    NSString*           _saveMsg;
    NSString*           _headMsg;     /* shown in place of group 0's header (restart steps, errors) */
}

static UIButton* menu_button(NSString* title)
{
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
    b.titleLabel.adjustsFontSizeToFitWidth = YES;
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [b setTitleColor:[UIColor colorWithWhite:1 alpha:0.35] forState:UIControlStateDisabled];
    b.backgroundColor = [UIColor colorWithWhite:1 alpha:0.12];
    b.layer.cornerRadius = 9;
    return b;
}

static UILabel* menu_label(const char* text)
{
    UILabel* l = [[UILabel alloc] init];
    l.text = [NSString stringWithUTF8String:text];
    l.textColor = UIColor.whiteColor;
    l.font = [UIFont systemFontOfSize:17];
    return l;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    _startScale = running_value(ROW_RESOLUTION);
    _ss = port_ss_available() != 0;
    _loadPick = -1;

    _panel = [[UIView alloc] init];
    _panel.backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.94];
    _panel.layer.cornerRadius = 16;
    _panel.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.15].CGColor;
    _panel.layer.borderWidth = 1;
    [self addSubview:_panel];

    _title = [[UILabel alloc] init];
    _title.text = @"Paused";
    _title.textColor = UIColor.whiteColor;
    _title.font = [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
    _title.textAlignment = NSTextAlignmentCenter;
    [_panel addSubview:_title];

    /* the group boxes first: they sit behind their rows */
    for (int g = 0; g < 2; g++) {
        _box[g] = [[UIView alloc] init];
        _box[g].backgroundColor = [UIColor colorWithWhite:1 alpha:0.05];
        _box[g].layer.cornerRadius = 12;
        _box[g].layer.borderColor = UIColor.systemOrangeColor.CGColor;
        [_panel addSubview:_box[g]];
        _head[g] = [[UILabel alloc] init];
        _head[g].font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
        _head[g].adjustsFontSizeToFitWidth = YES;
        _head[g].minimumScaleFactor = 0.7;
        _head[g].textColor = [UIColor colorWithWhite:1 alpha:0.6];
        [_panel addSubview:_head[g]];
    }
    _head[1].text = @"Graphics";

    for (int i = 0; i < NROWS; i++) {
        _label[i] = menu_label(kRows[i].title);
        [_panel addSubview:_label[i]];
        NSMutableArray* items = [NSMutableArray array];
        for (int k = 0; k < kRows[i].n; k++) [items addObject:[NSString stringWithUTF8String:kRows[i].names[k]]];
        _seg[i] = [[UISegmentedControl alloc] initWithItems:items];
        _seg[i].tag = i;
        [_seg[i] addTarget:self action:@selector(segChanged:) forControlEvents:UIControlEventValueChanged];
        [_panel addSubview:_seg[i]];
    }

    if (_ss) {
        _ssLabel[0] = menu_label("Save state");
        _ssLabel[1] = menu_label("Load state");
        [_panel addSubview:_ssLabel[0]];
        [_panel addSubview:_ssLabel[1]];
        _saveBtn = menu_button(@"Save now");
        [_saveBtn addTarget:self action:@selector(saveTapped) forControlEvents:UIControlEventTouchUpInside];
        [_panel addSubview:_saveBtn];
        for (int k = 0; k < 2; k++) {
            _loadBtn[k] = menu_button(@"");
            _loadBtn[k].tag = k;
            [_loadBtn[k] addTarget:self action:@selector(loadTapped:) forControlEvents:UIControlEventTouchUpInside];
            [_panel addSubview:_loadBtn[k]];
        }
        if (!slot_date(0) && slot_date(1)) _loadSel = 1;
    }

    _restart = [UIButton buttonWithType:UIButtonTypeSystem];
    [_restart setTitle:@" Restart now" forState:UIControlStateNormal];
    [_restart setImage:[UIImage systemImageNamed:@"arrow.clockwise"] forState:UIControlStateNormal];
    _restart.tintColor = UIColor.blackColor;
    [_restart setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
    [_restart setTitleColor:[UIColor colorWithWhite:0 alpha:0.45] forState:UIControlStateDisabled];
    _restart.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
    _restart.titleLabel.adjustsFontSizeToFitWidth = YES;
    _restart.backgroundColor = UIColor.systemOrangeColor;
    _restart.layer.cornerRadius = 9;
    _restart.hidden = YES;
    [_restart addTarget:self action:@selector(restartTapped) forControlEvents:UIControlEventTouchUpInside];
    [_panel addSubview:_restart];

    _resume = [UIButton buttonWithType:UIButtonTypeSystem];
    [_resume setTitle:@"Resume" forState:UIControlStateNormal];
    _resume.titleLabel.font = [UIFont systemFontOfSize:19 weight:UIFontWeightSemibold];
    [_resume setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [_resume setTitleColor:[UIColor colorWithWhite:1 alpha:0.35] forState:UIControlStateDisabled];
    _resume.backgroundColor = [UIColor colorWithWhite:1 alpha:0.12];
    _resume.layer.cornerRadius = 10;
    [_resume addTarget:self action:@selector(resumeTapped) forControlEvents:UIControlEventTouchUpInside];
    [_panel addSubview:_resume];

    _focus = F_RESUME;
    [self refresh];
    return self;
}

- (void)placeRow:(int)i x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w h:(CGFloat)h lw:(CGFloat)lw
{
    _label[i].frame = CGRectMake(x, y, lw, h);
    _seg[i].frame = CGRectMake(x + lw, y + 7, w - lw, h - 14);
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGRect b = self.bounds;
    const int n0 = _ss ? 3 : 1, n1 = 2;
    const CGFloat pad = 14, titleH = 30, headH = 36, inset = 12, gap = 12, resumeH = 46, lw = 110;
    const CGFloat boxFixed = headH + 8;   /* header + top / bottom margins */
    /* short, wide screen (iPhone in landscape): the groups side by side */
    const BOOL wide = b.size.height < 560 && b.size.width >= 760;
    const CGFloat W = fmin(wide ? 860 : 560, b.size.width - 40);
    const CGFloat fixed = 2 * pad + titleH + 6 + gap + resumeH + (wide ? boxFixed : 2 * boxFixed + gap);
    const int nrows = wide ? (n0 > n1 ? n0 : n1) : n0 + n1;
    /* rows shrink to fit a short screen */
    const CGFloat rowH = fmax(36, fmin(46, floor((b.size.height - 24 - fixed) / nrows)));
    const CGFloat H = fixed + nrows * rowH;
    _panel.frame = CGRectMake(floor((b.size.width - W) / 2), floor((b.size.height - H) / 2), W, H);
    _title.frame = CGRectMake(pad, pad, W - 2 * pad, titleH);

    const CGFloat top = pad + titleH + 6;
    CGRect r0, r1;
    if (wide) {
        const CGFloat bw = floor((W - 2 * pad - gap) / 2), bh = boxFixed + nrows * rowH;
        r0 = CGRectMake(pad, top, bw, bh);
        r1 = CGRectMake(pad + bw + gap, top, W - 2 * pad - gap - bw, bh);
    } else {
        r0 = CGRectMake(pad, top, W - 2 * pad, boxFixed + n0 * rowH);
        r1 = CGRectMake(pad, CGRectGetMaxY(r0) + gap, W - 2 * pad, boxFixed + n1 * rowH);
    }
    _box[0].frame = r0;
    _box[1].frame = r1;

    /* group 0: header with Restart now, then Save, Load, Resolution */
    const CGFloat x0 = r0.origin.x + inset, w0 = r0.size.width - 2 * inset;
    const CGFloat pillW = fmin(160, floor(w0 * 0.42));
    _restart.frame = CGRectMake(x0 + w0 - pillW, r0.origin.y + 6, pillW, headH - 6);
    _head[0].frame = CGRectMake(x0, r0.origin.y + 2, w0 - pillW - 10, headH);
    CGFloat y = r0.origin.y + headH + 2;
    if (_ss) {
        const CGFloat x = x0 + lw, w = w0 - lw, g = 8, half = floor((w - g) / 2);
        _ssLabel[0].frame = CGRectMake(x0, y, lw, rowH);
        _saveBtn.frame = CGRectMake(x, y + 6, w, rowH - 12);
        y += rowH;
        _ssLabel[1].frame = CGRectMake(x0, y, lw, rowH);
        _loadBtn[0].frame = CGRectMake(x, y + 6, half, rowH - 12);
        _loadBtn[1].frame = CGRectMake(x + half + g, y + 6, w - half - g, rowH - 12);
        y += rowH;
    }
    [self placeRow:ROW_RESOLUTION x:x0 y:y w:w0 h:rowH lw:lw];

    /* group 1: Shadows, Upscaling */
    const CGFloat x1 = r1.origin.x + inset, w1 = r1.size.width - 2 * inset;
    _head[1].frame = CGRectMake(x1, r1.origin.y + 2, w1, headH);
    y = r1.origin.y + headH + 2;
    [self placeRow:ROW_SHADOWS x:x1 y:y w:w1 h:rowH lw:lw];
    [self placeRow:ROW_UPSCALING x:x1 y:y + rowH w:w1 h:rowH lw:lw];

    _resume.frame = CGRectMake(pad, H - pad - resumeH, W - 2 * pad, resumeH);
}

- (double)wantScale { return kRows[ROW_RESOLUTION].values[_seg[ROW_RESOLUTION].selectedSegmentIndex]; }
- (BOOL)resChanged { return fabs([self wantScale] - _startScale) >= 0.01; }
- (BOOL)pending { return _loadPick >= 0 || [self resChanged]; }

/* What Restart now will do, for group 0's header. */
- (NSString*)restartText
{
    NSString* res = [NSString stringWithUTF8String:kRows[ROW_RESOLUTION].names[_seg[ROW_RESOLUTION].selectedSegmentIndex]];
    NSDate* d = _loadPick >= 0 ? slot_date(_loadPick) : nil;
    NSString* save = d ? [NSString stringWithFormat:@"%s %@", kSlotName[_loadPick], short_time(d)] : nil;
    if (save && [self resChanged]) return [NSString stringWithFormat:@"Restart to load %@ at %@", save, res];
    if (save) return [NSString stringWithFormat:@"Restart to load %@", save];
    return [NSString stringWithFormat:@"Restart to switch to %@", res];
}

- (BOOL)usable:(int)f
{
    if (f == F_RESTART) return [self pending];
    if (f == F_SAVE || f == F_LOAD) return _ss;
    return YES;
}

- (void)refresh
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    for (int i = 0; i < NROWS; i++) {
        id v = [d objectForKey:kRows[i].key];
        const double val = [v respondsToSelector:@selector(doubleValue)] ? [v doubleValue] : running_value(i);
        _seg[i].selectedSegmentIndex = index_of(&kRows[i], val);
        _seg[i].enabled = !_restarting;
    }
    if (_loadPick >= 0 && !slot_date(_loadPick)) _loadPick = -1;
    const BOOL pending = [self pending];
    if (![self usable:_focus]) _focus = _ss ? F_SAVE : F_RESOLUTION;

    UIColor* on = [UIColor colorWithWhite:1 alpha:0.9];
    for (int i = 0; i < NROWS; i++) {
        _seg[i].layer.borderWidth = _focus == row_focus(i) ? 2 : 0;
        _seg[i].layer.borderColor = on.CGColor;
        _seg[i].layer.cornerRadius = 9;
    }
    const int busy = _ss && port_ss_save_busy();
    if (_ss) {
        [_saveBtn setTitle:busy ? @"Saving..." : _saveMsg ? _saveMsg : @"Save now" forState:UIControlStateNormal];
        _saveBtn.enabled = !busy && !_restarting;
        _saveBtn.layer.borderWidth = _focus == F_SAVE ? 2 : 0;
        _saveBtn.layer.borderColor = on.CGColor;
        for (int k = 0; k < 2; k++) {
            NSDate* sd = slot_date(k);
            NSString* t = sd ? [NSString stringWithFormat:@"%@%s %@", _loadPick == k ? @"✓ " : @"", kSlotName[k], short_time(sd)]
                             : [NSString stringWithFormat:@"No %s", k ? "auto save" : "save"];
            [_loadBtn[k] setTitle:t forState:UIControlStateNormal];
            _loadBtn[k].enabled = sd != nil && !_restarting;
            _loadBtn[k].backgroundColor = _loadPick == k ? [UIColor.systemOrangeColor colorWithAlphaComponent:0.45]
                                                         : [UIColor colorWithWhite:1 alpha:0.12];
            _loadBtn[k].layer.borderWidth = (_focus == F_LOAD && _loadSel == k) ? 2 : 0;
            _loadBtn[k].layer.borderColor = on.CGColor;
        }
    }

    _restart.hidden = !pending;
    _restart.enabled = !busy && !_restarting;
    [_restart setTitle:_restarting ? @" Closing..." : @" Restart now" forState:UIControlStateNormal];
    _restart.layer.borderWidth = _focus == F_RESTART ? 3 : 0;
    _restart.layer.borderColor = UIColor.whiteColor.CGColor;
    _box[0].layer.borderWidth = pending ? 2 : 0;
    if (_headMsg) {
        _head[0].text = _headMsg;
        _head[0].textColor = UIColor.whiteColor;
    } else if (pending) {
        _head[0].text = [self restartText];
        _head[0].textColor = UIColor.systemOrangeColor;
    } else {
        _head[0].text = _ss ? @"Saves and resolution" : @"Needs a restart";
        _head[0].textColor = [UIColor colorWithWhite:1 alpha:0.6];
    }
    if (pending && !_wasPending) {   /* draw the eye when it shows up */
        CABasicAnimation* a = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
        a.fromValue = @1.0;
        a.toValue = @1.12;
        a.duration = 0.16;
        a.autoreverses = YES;
        a.repeatCount = 2;
        [_restart.layer addAnimation:a forKey:@"pulse"];
    }
    _wasPending = pending;

    _resume.enabled = !_restarting;
    _resume.layer.borderWidth = _focus == F_RESUME ? 2 : 0;
    _resume.layer.borderColor = on.CGColor;
}

- (void)setHeadMessage:(NSString*)msg
{
    _headMsg = msg;
    [self refresh];
}

- (void)store:(int)row
{
    const double v = kRows[row].values[_seg[row].selectedSegmentIndex];
    if (row == ROW_RESOLUTION) [NSUserDefaults.standardUserDefaults setDouble:v forKey:kRows[row].key];
    else [NSUserDefaults.standardUserDefaults setInteger:(NSInteger)v forKey:kRows[row].key];
    port_ios_gfx_apply_live();
    _headMsg = nil;
    [self refresh];
}

- (void)segChanged:(UISegmentedControl*)s
{
    _focus = row_focus((int)s.tag);
    [self store:(int)s.tag];
}

- (void)move:(int)d
{
    if (_restarting) return;
    do _focus = (_focus + d + NFOCUS) % NFOCUS;
    while (![self usable:_focus]);
    [self refresh];
}

- (void)change:(int)d
{
    if (_restarting) return;
    if (_focus == F_LOAD) {
        const int k = _loadSel + d;
        if (k < 0 || k > 1) return;
        _loadSel = k;
        [self refresh];
        return;
    }
    const int row = focus_row(_focus);
    if (row < 0) return;
    UISegmentedControl* s = _seg[row];
    const int k = (int)s.selectedSegmentIndex + d;
    if (k < 0 || k >= (int)s.numberOfSegments) return;
    s.selectedSegmentIndex = k;
    [self store:row];
}

- (void)resumeTapped
{
    port_game_menu_close();
}

/* Cross on Restart now, Save or Load (gamepad) */
- (void)activate
{
    if (_focus == F_RESTART) [self restartTapped];
    else if (_focus == F_SAVE) [self saveTapped];
    else if (_focus == F_LOAD) [self pickSlot:_loadSel];
}

- (void)saveTapped
{
    _focus = F_SAVE;
    if (_restarting || port_ss_save_busy()) return;
    _saveMsg = nil;
    if (!port_ss_save_async(slot_path(0).fileSystemRepresentation, menu_save_done, NULL))
        _saveMsg = @"Busy, try again";
    else
        fprintf(stderr, "[ios] menu: save state\n");
    [self refresh];
}

- (void)saveDone:(int)ok
{
    _saveMsg = ok ? [NSString stringWithFormat:@"Saved %@", short_time(NSDate.date)] : @"Save failed, try again";
    [self refresh];
}

- (void)loadTapped:(UIButton*)b
{
    [self pickSlot:(int)b.tag];
}

/* Picks the save the restart loads; the same slot again un-picks it. */
- (void)pickSlot:(int)slot
{
    if (_restarting) return;
    _focus = F_LOAD;
    _loadSel = slot;
    _headMsg = nil;
    if (slot_date(slot)) {
        _loadPick = _loadPick == slot ? -1 : slot;
        fprintf(stderr, "[ios] menu: load at restart: %s\n", _loadPick >= 0 ? kSlotFile[slot] : "none");
    }
    [self refresh];
}

/* Test (MENU_SCRIPT res<N>): the same as a tap on that Resolution choice. */
- (void)pickResolution:(int)index
{
    if (_restarting || index < 0 || index >= kRows[ROW_RESOLUTION].n) return;
    _focus = F_RESOLUTION;
    _seg[ROW_RESOLUTION].selectedSegmentIndex = index;
    [self store:ROW_RESOLUTION];
}

- (void)restartTapped
{
    if (_restarting || ![self pending]) return;
    _focus = F_RESTART;
    if (_ss && port_ss_save_busy()) { [self refresh]; return; }   /* a save is still running */
    if (_loadPick >= 0 && !port_ss_load_next(kSlotFile[_loadPick])) {
        _loadPick = -1;
        [self setHeadMessage:@"Load failed. Try again."];
        return;
    }
    [NSUserDefaults.standardUserDefaults synchronize];   /* the app ends with _exit: write Resolution now */
    _restarting = YES;
    fprintf(stderr, "[ios] menu: restart now (load %s, render scale %.1f)\n",
            _loadPick >= 0 ? kSlotFile[_loadPick] : "none", [self wantScale]);
    [self refresh];
    menu_reopen_then_exit(_loadPick >= 0 ? @"It goes on from your save."
        : [NSString stringWithFormat:@"It starts at %s.", kRows[ROW_RESOLUTION].names[_seg[ROW_RESOLUTION].selectedSegmentIndex]]);
}
@end

/* ---- shell interface ---------------------------------------------------- */

static UIWindow*     s_win;
static int           s_close_btn = -1;   /* the gamepad button whose release closes the menu */
static int           s_stick_axis;       /* left stick: 1 x, 2 y after a step, 0 once it centres */

extern "C" int port_game_menu_open(void) { return s_menu != nil; }

/* 1 while Restart now runs (ios_shell.mm: no background save over a picked save then;
 * the notification question sends the app the "going to the background" event). */
extern "C" int port_game_menu_restarting(void) { return s_menu && s_menu.restarting; }

extern "C" void port_game_menu_close(void)
{
    if (!s_menu || s_menu.restarting) return;
    [s_menu removeFromSuperview];
    s_menu = nil;
    s_close_btn = -1;
    port_touch_pad_menu(0);
    port_app_set_paused(0);
    fprintf(stderr, "[ios] menu closed: game resumed\n");
}

extern "C" void port_game_menu_show(void)
{
    if (s_menu || !s_win) return;
    UIView* host = s_win.rootViewController.view ? s_win.rootViewController.view : s_win;
    port_app_set_paused(2);
    port_touch_pad_menu(1);
    s_menu = [[PortGameMenu alloc] initWithFrame:host.bounds];
    [host addSubview:s_menu];
    s_close_btn = -1;
    s_stick_axis = 0;
    fprintf(stderr, "[ios] menu open: game paused\n");
}

/* Test (MENU_SCRIPT, ios_shell.mm), main thread: the same as a tap. */
extern "C" void port_game_menu_test(const char* what)
{
    if (!strcmp(what, "open")) port_game_menu_show();
    else if (!strcmp(what, "close")) port_game_menu_close();
    else if (!s_menu) fprintf(stderr, "[ios] menu script: %s needs the menu open\n", what);
    else if (!strcmp(what, "save")) [s_menu saveTapped];
    else if (!strcmp(what, "load0") || !strcmp(what, "load1")) [s_menu pickSlot:what[4] - '0'];
    else if (!strncmp(what, "res", 3) && what[3] >= '0' && what[3] <= '9') [s_menu pickResolution:what[3] - '0'];
    else if (!strcmp(what, "restart")) [s_menu restartTapped];
}

/* Main thread (ios_shell.mm), at start: clears a "tap to open" notice left from the
 * restart that opened this run. */
extern "C" void port_game_menu_attach(void* uiwindow)
{
    s_win = (__bridge UIWindow*)uiwindow;
    UNUserNotificationCenter* c = UNUserNotificationCenter.currentNotificationCenter;
    [c removePendingNotificationRequestsWithIdentifiers:@[ kReopenNote ]];
    [c removeDeliveredNotificationsWithIdentifiers:@[ kReopenNote ]];
}

/* Main thread, from the SDL event watch. Returns 1 when the menu used the event. */
extern "C" int port_game_menu_gamepad_event(const SDL_Event* e)
{
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const int b = e->gbutton.button;
        if (!s_menu) {
            if (b == SDL_GAMEPAD_BUTTON_GUIDE || b == SDL_GAMEPAD_BUTTON_TOUCHPAD) { port_game_menu_show(); return 1; }
            return 0;
        }
        switch (b) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:    [s_menu move:-1]; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  [s_menu move:+1]; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  [s_menu change:-1]; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: [s_menu change:+1]; break;
        case SDL_GAMEPAD_BUTTON_SOUTH:      if (s_menu.focus == F_RESUME) s_close_btn = b; else [s_menu activate]; break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_START:
        case SDL_GAMEPAD_BUTTON_GUIDE:
        case SDL_GAMEPAD_BUTTON_TOUCHPAD:   s_close_btn = b; break;
        default: break;
        }
        return 1;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (s_menu && e->gbutton.button == s_close_btn) port_game_menu_close();
        return s_menu != nil;
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && s_menu &&
        (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        const int v = e->gaxis.value, x = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX, axis = x ? 1 : 2;
        if (abs(v) < 8000) { if (s_stick_axis == axis) s_stick_axis = 0; return 1; }
        if (abs(v) < 20000 || s_stick_axis) return 1;
        s_stick_axis = axis;
        if (x) [s_menu change:v > 0 ? +1 : -1];
        else   [s_menu move:v > 0 ? +1 : -1];
        return 1;
    }
    return 0;
}
