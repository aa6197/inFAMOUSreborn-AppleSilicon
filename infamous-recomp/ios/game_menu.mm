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
 */
#include <SDL3/SDL.h>
#import <UIKit/UIKit.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" void port_app_set_paused(int paused);   /* main.cpp: 0 run, 1 background, 2 menu */
extern "C" void port_touch_pad_menu(int open);     /* touch_pad.mm */
extern "C" void port_ios_gfx_apply_live(void);     /* ios_shell.mm */
extern "C" int  rsx_engine_shadow_maps(void);
extern "C" void port_game_menu_close(void);

typedef struct { const char* title; NSString* key; int n; const char* names[5]; double values[5]; } MenuRow;
static const MenuRow kRows[] = {
    { "Shadows",    @"Shadows",     3, { "On", "Near only", "Off" },                     { 2, 1, 0 } },
    { "Resolution", @"RenderScale", 5, { "720p", "630p", "540p", "450p", "360p" },        { 100, 87.5, 75, 62.5, 50 } },
    { "Upscaling",  @"Upscaling",   2, { "MetalFX", "Simple" },                          { 1, 0 } },
};
enum { NROWS = sizeof kRows / sizeof kRows[0], ROW_RESUME = NROWS, ROW_RESOLUTION = 1 };

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
@end

@implementation PortGameMenu {
    UIView*             _panel;
    UILabel*            _title;
    UILabel*            _label[NROWS];
    UISegmentedControl* _seg[NROWS];
    UIButton*           _resume;
    UILabel*            _note;
    double              _startScale;
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
    const CGFloat rowH = 46, pad = 20, lw = 120;
    const CGFloat H = pad + 34 + 8 + (CGFloat)NROWS * rowH + 24 + 12 + 50 + pad;
    _panel.frame = CGRectMake(floor((b.size.width - W) / 2), floor((b.size.height - H) / 2), W, H);
    _title.frame = CGRectMake(pad, pad, W - 2 * pad, 34);
    CGFloat y = pad + 34 + 8;
    for (int i = 0; i < NROWS; i++) {
        _label[i].frame = CGRectMake(pad, y, lw, rowH);
        _seg[i].frame = CGRectMake(pad + lw, y + 7, W - 2 * pad - lw, rowH - 14);
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
    _note.hidden = fabs(want - _startScale) < 0.01;
    UIColor* on = [UIColor colorWithWhite:1 alpha:0.9];
    for (int i = 0; i < NROWS; i++) {
        _seg[i].layer.borderWidth = _focus == i ? 2 : 0;
        _seg[i].layer.borderColor = on.CGColor;
        _seg[i].layer.cornerRadius = 9;
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
    _focus = (_focus + d + NROWS + 1) % (NROWS + 1);
    [self refresh];
}

- (void)change:(int)d
{
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
@end

/* ---- shell interface ---------------------------------------------------- */

static UIWindow*     s_win;
static PortGameMenu* s_menu;
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
        case SDL_GAMEPAD_BUTTON_SOUTH:      if (s_menu.focus == ROW_RESUME) s_close_btn = b; break;
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
