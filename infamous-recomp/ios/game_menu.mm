/* In-game menu for the iOS / iPadOS app (Apple port).
 *
 * Opens with the menu button on the touch controls (touch_pad.mm), or the
 * Home button (PS / Xbox / Guide) or the touchpad click on a gamepad. The game
 * pauses while it is up (port_app_set_paused(2): the frame clock parks, GPU
 * memory is kept).
 *
 * It holds the Graphics choices of the Settings app, with the same keys
 * (apple/gfx_settings.h), so both always agree. Shadows and Upscaling change
 * at once; Resolution at the next start.
 *
 * Gamepad: up / down picks a row, left / right changes it, Cross on Resume,
 * or Circle / START / Home anywhere, closes it. It closes when that button is
 * let go, so the game never sees the press.
 *
 * Save-state builds (PORT_SAVE_STATES, port_ss_available()) add two rows:
 * "Save state" saves to Documents/save.ss while the game stays paused, and
 * "Load state" picks that save or the one made when the app last went to the
 * background (auto.ss, ios_shell.mm). A load only works at a fresh start: the
 * first press arms it, the second leaves a marker (port_ss_load_next) and the
 * app closes; opening it again loads the save. Other builds: no change.
 */
#include <SDL3/SDL.h>
#import <UIKit/UIKit.h>
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
enum { NROWS = sizeof kRows / sizeof kRows[0], ROW_SAVE = NROWS, ROW_LOAD = NROWS + 1, ROW_RESUME = NROWS + 2,
       NFOCUS = NROWS + 3, ROW_RESOLUTION = 1 };

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
    case 0: { const int n = rsx_engine_shadow_maps(); return n < 0 ? 2 : n == 0 ? 0 : 1; }
    case 1: return (e = getenv("RENDER_SCALE")) && *e ? atof(e) : 100;
    default: return (e = getenv("METALFX")) && *e == '0' ? 0 : 1;
    }
}

@interface PortGameMenu : UIView
@property(nonatomic) int focus;
- (void)refresh;
- (void)move:(int)d;
- (void)change:(int)d;
- (void)activate;
- (void)saveDone:(int)ok;
- (void)saveTapped;
- (void)loadSlot:(int)slot;
@end

static PortGameMenu* s_menu;

/* save thread -> main thread */
static void menu_save_done(int ok, void*)
{
    dispatch_async(dispatch_get_main_queue(), ^{ [s_menu saveDone:ok]; });
}

@implementation PortGameMenu {
    UIView*             _panel;
    UILabel*            _title;
    UILabel*            _label[NROWS];
    UISegmentedControl* _seg[NROWS];
    UIButton*           _resume;
    UILabel*            _note;
    double              _startScale;
    BOOL                _ss;          /* save-state build: Save / Load rows */
    UILabel*            _ssLabel[2];
    UIButton*           _saveBtn;
    UIButton*           _loadBtn[2];
    int                 _loadSel;     /* slot the Load row points at (gamepad) */
    int                 _armed;       /* slot whose load waits for a second press, or -1 */
    int                 _armSeq;
    NSString*           _saveMsg;
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

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    _startScale = running_value(ROW_RESOLUTION);

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

    for (int i = 0; i < NROWS; i++) {
        _label[i] = [[UILabel alloc] init];
        _label[i].text = [NSString stringWithUTF8String:kRows[i].title];
        _label[i].textColor = UIColor.whiteColor;
        _label[i].font = [UIFont systemFontOfSize:17];
        [_panel addSubview:_label[i]];
        NSMutableArray* items = [NSMutableArray array];
        for (int k = 0; k < kRows[i].n; k++) [items addObject:[NSString stringWithUTF8String:kRows[i].names[k]]];
        _seg[i] = [[UISegmentedControl alloc] initWithItems:items];
        _seg[i].tag = i;
        [_seg[i] addTarget:self action:@selector(segChanged:) forControlEvents:UIControlEventValueChanged];
        [_panel addSubview:_seg[i]];
    }

    _note = [[UILabel alloc] init];
    _note.text = @"Resolution changes at the next start.";
    _note.textColor = [UIColor colorWithWhite:1 alpha:0.6];
    _note.font = [UIFont systemFontOfSize:13];
    _note.textAlignment = NSTextAlignmentCenter;
    [_panel addSubview:_note];

    _ss = port_ss_available() != 0;
    _armed = -1;
    _loadSel = 0;
    if (_ss) {
        const char* names[2] = { "Save state", "Load state" };
        for (int i = 0; i < 2; i++) {
            _ssLabel[i] = [[UILabel alloc] init];
            _ssLabel[i].text = [NSString stringWithUTF8String:names[i]];
            _ssLabel[i].textColor = UIColor.whiteColor;
            _ssLabel[i].font = [UIFont systemFontOfSize:17];
            [_panel addSubview:_ssLabel[i]];
        }
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

    _resume = [UIButton buttonWithType:UIButtonTypeSystem];
    [_resume setTitle:@"Resume" forState:UIControlStateNormal];
    _resume.titleLabel.font = [UIFont systemFontOfSize:19 weight:UIFontWeightSemibold];
    [_resume setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    _resume.backgroundColor = [UIColor colorWithWhite:1 alpha:0.12];
    _resume.layer.cornerRadius = 10;
    [_resume addTarget:self action:@selector(resumeTapped) forControlEvents:UIControlEventTouchUpInside];
    [_panel addSubview:_resume];

    _focus = ROW_RESUME;
    [self refresh];
    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGRect b = self.bounds;
    const CGFloat W = fmin(560, b.size.width - 40);
    const CGFloat pad = 20, lw = 120, nrows = (CGFloat)(NROWS + (_ss ? 2 : 0));
    /* rows shrink to fit a short screen (iPhone in landscape: ~400 pt) */
    const CGFloat rowH = fmax(36, fmin(46, floor((b.size.height - 24 - (2 * pad + 34 + 8 + 24 + 12 + 50)) / nrows)));
    const CGFloat H = pad + 34 + 8 + nrows * rowH + 24 + 12 + 50 + pad;
    _panel.frame = CGRectMake(floor((b.size.width - W) / 2), floor((b.size.height - H) / 2), W, H);
    _title.frame = CGRectMake(pad, pad, W - 2 * pad, 34);
    CGFloat y = pad + 34 + 8;
    for (int i = 0; i < NROWS; i++) {
        _label[i].frame = CGRectMake(pad, y, lw, rowH);
        _seg[i].frame = CGRectMake(pad + lw, y + 7, W - 2 * pad - lw, rowH - 14);
        y += rowH;
    }
    if (_ss) {
        const CGFloat x = pad + lw, w = W - 2 * pad - lw, gap = 8;
        _ssLabel[0].frame = CGRectMake(pad, y, lw, rowH);
        _saveBtn.frame = CGRectMake(x, y + 6, w, rowH - 12);
        y += rowH;
        _ssLabel[1].frame = CGRectMake(pad, y, lw, rowH);
        _loadBtn[0].frame = CGRectMake(x, y + 6, floor((w - gap) / 2), rowH - 12);
        _loadBtn[1].frame = CGRectMake(x + floor((w - gap) / 2) + gap, y + 6, w - floor((w - gap) / 2) - gap, rowH - 12);
        y += rowH;
    }
    _note.frame = CGRectMake(pad, y, W - 2 * pad, 24);
    y += 24 + 12;
    _resume.frame = CGRectMake(pad, y, W - 2 * pad, 50);
}

- (void)refresh
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    for (int i = 0; i < NROWS; i++) {
        id v = [d objectForKey:kRows[i].key];
        const double val = [v respondsToSelector:@selector(doubleValue)] ? [v doubleValue] : running_value(i);
        _seg[i].selectedSegmentIndex = index_of(&kRows[i], val);
    }
    const double want = kRows[ROW_RESOLUTION].values[_seg[ROW_RESOLUTION].selectedSegmentIndex];
    _note.text = @"Resolution changes at the next start.";
    _note.hidden = fabs(want - _startScale) < 0.01;
    UIColor* on = [UIColor colorWithWhite:1 alpha:0.9];
    for (int i = 0; i < NROWS; i++) {
        _seg[i].layer.borderWidth = _focus == i ? 2 : 0;
        _seg[i].layer.borderColor = on.CGColor;
        _seg[i].layer.cornerRadius = 9;
    }
    if (_ss) {
        const int busy = port_ss_save_busy();
        [_saveBtn setTitle:busy ? @"Saving..." : _saveMsg ? _saveMsg : @"Save now" forState:UIControlStateNormal];
        _saveBtn.enabled = !busy;
        _saveBtn.layer.borderWidth = _focus == ROW_SAVE ? 2 : 0;
        _saveBtn.layer.borderColor = on.CGColor;
        for (int k = 0; k < 2; k++) {
            NSDate* d = slot_date(k);
            NSString* t = _armed == k ? @"Press again to load"
                        : d ? [NSString stringWithFormat:@"%s %@", kSlotName[k], short_time(d)]
                            : [NSString stringWithFormat:@"No %s", k ? "auto save" : "save"];
            [_loadBtn[k] setTitle:t forState:UIControlStateNormal];
            _loadBtn[k].enabled = d != nil && !busy;
            _loadBtn[k].layer.borderWidth = (_focus == ROW_LOAD && _loadSel == k) ? 2 : 0;
            _loadBtn[k].layer.borderColor = on.CGColor;
        }
        if (_armed >= 0) {
            _note.text = @"The game closes. Open it again to go on from the save.";
            _note.hidden = NO;
        }
    }
    _resume.layer.borderWidth = _focus == ROW_RESUME ? 2 : 0;
    _resume.layer.borderColor = on.CGColor;
}

- (void)store:(int)row
{
    const double v = kRows[row].values[_seg[row].selectedSegmentIndex];
    if (row == ROW_RESOLUTION) [NSUserDefaults.standardUserDefaults setDouble:v forKey:kRows[row].key];
    else [NSUserDefaults.standardUserDefaults setInteger:(NSInteger)v forKey:kRows[row].key];
    port_ios_gfx_apply_live();
    [self refresh];
}

- (void)segChanged:(UISegmentedControl*)s
{
    _focus = (int)s.tag;
    [self store:(int)s.tag];
}

- (void)move:(int)d
{
    do _focus = (_focus + d + NFOCUS) % NFOCUS;
    while (!_ss && (_focus == ROW_SAVE || _focus == ROW_LOAD));
    [self refresh];
}

- (void)change:(int)d
{
    if (_focus == ROW_LOAD) {
        const int k = _loadSel + d;
        if (k < 0 || k > 1) return;
        _loadSel = k;
        _armed = -1;
        [self refresh];
        return;
    }
    if (_focus >= NROWS) return;
    UISegmentedControl* s = _seg[_focus];
    const int k = (int)s.selectedSegmentIndex + d;
    if (k < 0 || k >= (int)s.numberOfSegments) return;
    s.selectedSegmentIndex = k;
    [self store:_focus];
}

- (void)resumeTapped
{
    port_game_menu_close();
}

/* Cross on a Save / Load row (gamepad) */
- (void)activate
{
    if (_focus == ROW_SAVE) [self saveTapped];
    else if (_focus == ROW_LOAD) [self loadSlot:_loadSel];
}

- (void)saveTapped
{
    _focus = ROW_SAVE;
    _armed = -1;
    if (port_ss_save_busy()) return;
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
    [self loadSlot:(int)b.tag];
}

- (void)loadSlot:(int)slot
{
    _focus = ROW_LOAD;
    _loadSel = slot;
    if (!slot_date(slot) || port_ss_save_busy()) { [self refresh]; return; }
    if (_armed != slot) {   /* first press: arm for 4 s */
        _armed = slot;
        const int seq = ++_armSeq;
        __weak PortGameMenu* me = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            PortGameMenu* m = me;
            if (m && m->_armSeq == seq && m->_armed >= 0) { m->_armed = -1; [m refresh]; }
        });
        [self refresh];
        return;
    }
    if (!port_ss_load_next(kSlotFile[slot])) {
        _armed = -1;
        _saveMsg = @"Load failed";
        [self refresh];
        return;
    }
    fprintf(stderr, "[ios] menu: load %s at the next start; closing the app\n", kSlotFile[slot]);
    fflush(stderr);
    _exit(0);
}
@end

/* ---- shell interface ---------------------------------------------------- */

static UIWindow*     s_win;
static int           s_close_btn = -1;   /* the gamepad button whose release closes the menu */
static int           s_stick_axis;       /* left stick: 1 x, 2 y after a step, 0 once it centres */

extern "C" int port_game_menu_open(void) { return s_menu != nil; }

extern "C" void port_game_menu_close(void)
{
    if (!s_menu) return;
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
    else if (!strcmp(what, "load0") || !strcmp(what, "load1")) [s_menu loadSlot:what[4] - '0'];
}

/* Main thread (ios_shell.mm). */
extern "C" void port_game_menu_attach(void* uiwindow) { s_win = (__bridge UIWindow*)uiwindow; }

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
        case SDL_GAMEPAD_BUTTON_SOUTH:      if (s_menu.focus == ROW_RESUME) s_close_btn = b; else [s_menu activate]; break;
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
