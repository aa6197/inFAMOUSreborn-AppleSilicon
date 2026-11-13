/* On-screen controller for the iOS / iPadOS app (Apple port).
 *
 * A transparent UIKit view over the game's Metal view. It shows when no
 * gamepad is connected and hides when one is (TOUCH_PAD=1 always shows it,
 * TOUCH_PAD=0 never). Its state goes to cellPad (cellPad_set_touch_state),
 * which feeds it to port 0 next to any real gamepad.
 *
 * Layout, landscape: L1 L2 top left, R2 R1 top right, SELECT START top
 * centre, a D-pad on the left under the shoulders, the face buttons bottom
 * right. A touch anywhere else on the left half is a floating left stick
 * (movement); on the right half a floating right stick (camera). Fingers can
 * slide from one face button to the next. The small button under SELECT /
 * START opens the in-game menu (game_menu.mm); the pad hides while it is up.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <stdlib.h>

extern "C" void cellPad_set_touch_state(int on, unsigned buttons, unsigned lx, unsigned ly,
                                        unsigned rx, unsigned ry);
extern "C" void port_game_menu_show(void);   /* game_menu.mm */

/* CELL_PAD_CTRL_* (libs/input/cellPad.h) */
enum : unsigned {
    B_SELECT = 1u << 0, B_L3 = 1u << 1, B_R3 = 1u << 2, B_START = 1u << 3,
    B_UP = 1u << 4, B_RIGHT = 1u << 5, B_DOWN = 1u << 6, B_LEFT = 1u << 7,
    B_L2 = 1u << 8, B_R2 = 1u << 9, B_L1 = 1u << 10, B_R1 = 1u << 11,
    B_TRIANGLE = 1u << 12, B_CIRCLE = 1u << 13, B_CROSS = 1u << 14, B_SQUARE = 1u << 15,
};

typedef struct { const char* label; unsigned mask; int round; } PadButtonDef;
static const PadButtonDef kButtons[] = {
    { "△", B_TRIANGLE, 1 }, { "○", B_CIRCLE, 1 }, { "✕", B_CROSS, 1 }, { "□", B_SQUARE, 1 },
    { "▲", B_UP, 1 }, { "▶", B_RIGHT, 1 }, { "▼", B_DOWN, 1 }, { "◀", B_LEFT, 1 },
    { "L1", B_L1, 0 }, { "L2", B_L2, 0 }, { "R1", B_R1, 0 }, { "R2", B_R2, 0 },
    { "SELECT", B_SELECT, 0 }, { "START", B_START, 0 }, { "L3", B_L3, 1 }, { "R3", B_R3, 1 },
};
enum { NBUTTONS = sizeof kButtons / sizeof kButtons[0] };
enum { ROLE_NONE = -3, ROLE_LSTICK = -2, ROLE_RSTICK = -1 };   /* >= 0: a button index */

@interface PortTouchPad : UIView
@end

@implementation PortTouchPad {
    CGRect        _rect[NBUTTONS];
    CAShapeLayer* _shape[NBUTTONS];
    CATextLayer*  _text[NBUTTONS];
    CAShapeLayer* _stickBase[2];
    CAShapeLayer* _stickKnob[2];
    CGPoint       _stickOrigin[2];
    CGPoint       _stickPos[2];
    CGPoint       _stickHome[2];
    CGFloat       _stickRadius;
    NSMapTable*   _roles;            /* UITouch -> NSNumber role */
    unsigned      _shown;            /* last published buttons, to skip redraws */
    CGRect        _menuRect;
    CAShapeLayer* _menuShape;
    CATextLayer*  _menuText;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _roles = [NSMapTable strongToStrongObjectsMapTable];
    for (int i = 0; i < NBUTTONS; i++) {
        _shape[i] = [CAShapeLayer layer];
        _shape[i].fillColor = [UIColor colorWithWhite:1 alpha:0.18].CGColor;
        _shape[i].strokeColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
        _shape[i].lineWidth = 1.5;
        [self.layer addSublayer:_shape[i]];
        _text[i] = [CATextLayer layer];
        _text[i].string = [NSString stringWithUTF8String:kButtons[i].label];
        _text[i].alignmentMode = kCAAlignmentCenter;
        _text[i].foregroundColor = [UIColor colorWithWhite:1 alpha:0.75].CGColor;
        _text[i].contentsScale = UIScreen.mainScreen.scale;
        [self.layer addSublayer:_text[i]];
    }
    for (int s = 0; s < 2; s++) {
        _stickBase[s] = [CAShapeLayer layer];
        _stickBase[s].fillColor = [UIColor colorWithWhite:1 alpha:0.08].CGColor;
        _stickBase[s].strokeColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
        _stickBase[s].lineWidth = 1.5;
        [self.layer addSublayer:_stickBase[s]];
        _stickKnob[s] = [CAShapeLayer layer];
        _stickKnob[s].fillColor = [UIColor colorWithWhite:1 alpha:0.30].CGColor;
        [self.layer addSublayer:_stickKnob[s]];
    }
    _menuShape = [CAShapeLayer layer];
    _menuShape.fillColor = [UIColor colorWithWhite:1 alpha:0.18].CGColor;
    _menuShape.strokeColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    _menuShape.lineWidth = 1.5;
    [self.layer addSublayer:_menuShape];
    _menuText = [CATextLayer layer];
    _menuText.string = @"MENU";
    _menuText.alignmentMode = kCAAlignmentCenter;
    _menuText.foregroundColor = [UIColor colorWithWhite:1 alpha:0.75].CGColor;
    _menuText.contentsScale = UIScreen.mainScreen.scale;
    [self.layer addSublayer:_menuText];
    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGRect b = self.bounds;
    const UIEdgeInsets in = self.safeAreaInsets;
    const CGFloat W = b.size.width, H = b.size.height;
    const CGFloat u = fmax(40.0, fmin(W, H) * 0.085);   /* face button size */
    const CGFloat L = in.left + 0.3 * u, R = W - in.right - 0.3 * u;
    const CGFloat T = in.top + 0.3 * u, B = H - in.bottom - 0.3 * u;

    /* face buttons: a diamond in the bottom right */
    const CGPoint fc = CGPointMake(R - 1.9 * u, B - 1.9 * u);
    const CGFloat fd = 1.15 * u;
    const CGPoint face[4] = { {fc.x, fc.y - fd}, {fc.x + fd, fc.y}, {fc.x, fc.y + fd}, {fc.x - fd, fc.y} };
    for (int i = 0; i < 4; i++)
        _rect[i] = CGRectMake(face[i].x - 0.5 * u, face[i].y - 0.5 * u, u, u);

    /* D-pad on the left, under the shoulders */
    const CGFloat du = 0.8 * u;
    const CGPoint dc = CGPointMake(L + 1.6 * u, T + 1.4 * u + 1.5 * du);
    const CGPoint dp[4] = { {dc.x, dc.y - du}, {dc.x + du, dc.y}, {dc.x, dc.y + du}, {dc.x - du, dc.y} };
    for (int i = 0; i < 4; i++)
        _rect[4 + i] = CGRectMake(dp[i].x - 0.5 * du, dp[i].y - 0.5 * du, du, du);

    /* shoulders */
    const CGFloat sw = 1.7 * u, sh = 0.75 * u;
    _rect[8]  = CGRectMake(L, T, sw, sh);                       /* L1 */
    _rect[9]  = CGRectMake(L + sw + 0.3 * u, T, sw, sh);        /* L2 */
    _rect[10] = CGRectMake(R - sw, T, sw, sh);                  /* R1 */
    _rect[11] = CGRectMake(R - 2 * sw - 0.3 * u, T, sw, sh);    /* R2 */

    /* SELECT / START */
    const CGFloat pw = 1.6 * u, ph = 0.55 * u;
    _rect[12] = CGRectMake(W / 2 - pw - 0.25 * u, T, pw, ph);
    _rect[13] = CGRectMake(W / 2 + 0.25 * u, T, pw, ph);
    /* MENU: centred under them */
    _menuRect = CGRectMake(W / 2 - 0.6 * u, T + ph + 0.3 * u, 1.2 * u, 0.5 * u);

    /* L3 / R3: small, beside the stick homes */
    _stickRadius = 1.25 * u;
    _stickHome[0] = CGPointMake(L + 1.9 * u + _stickRadius * 0.6, B - _stickRadius - 0.2 * u);
    _stickHome[1] = CGPointMake(fc.x - 3.6 * u, B - _stickRadius - 0.2 * u);
    const CGFloat su = 0.6 * u;
    _rect[14] = CGRectMake(_stickHome[0].x + _stickRadius + 0.2 * u, B - su, su, su);
    _rect[15] = CGRectMake(_stickHome[1].x - _stickRadius - 0.2 * u - su, B - su, su, su);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBUTTONS; i++) {
        const CGRect r = _rect[i];
        UIBezierPath* p = kButtons[i].round ? [UIBezierPath bezierPathWithOvalInRect:r]
                                            : [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:r.size.height / 2];
        _shape[i].path = p.CGPath;
        const CGFloat fs = kButtons[i].round ? r.size.height * 0.45 : r.size.height * 0.42;
        _text[i].fontSize = fs;
        _text[i].frame = CGRectMake(r.origin.x, r.origin.y + (r.size.height - fs * 1.2) / 2, r.size.width, fs * 1.25);
    }
    _menuShape.path = [UIBezierPath bezierPathWithRoundedRect:_menuRect cornerRadius:_menuRect.size.height / 2].CGPath;
    const CGFloat mfs = _menuRect.size.height * 0.42;
    _menuText.fontSize = mfs;
    _menuText.frame = CGRectMake(_menuRect.origin.x, _menuRect.origin.y + (_menuRect.size.height - mfs * 1.2) / 2,
                                 _menuRect.size.width, mfs * 1.25);
    [CATransaction commit];
    for (int s = 0; s < 2; s++)
        if (![self stickHeld:s]) { _stickOrigin[s] = _stickPos[s] = _stickHome[s]; }
    [self redraw];
}

- (BOOL)stickHeld:(int)s
{
    for (UITouch* t in _roles) if ([[_roles objectForKey:t] intValue] == (s ? ROLE_RSTICK : ROLE_LSTICK)) return YES;
    return NO;
}

- (int)buttonAt:(CGPoint)p
{
    int best = -1; CGFloat bestd = 1e9;
    for (int i = 0; i < NBUTTONS; i++) {
        const CGRect r = CGRectInset(_rect[i], -0.18 * _rect[i].size.width, -0.18 * _rect[i].size.height);
        if (!CGRectContainsPoint(r, p)) continue;
        const CGFloat dx = p.x - CGRectGetMidX(r), dy = p.y - CGRectGetMidY(r), d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:self];
        if (CGRectContainsPoint(CGRectInset(_menuRect, -0.15 * _menuRect.size.width, -0.3 * _menuRect.size.height), p)) {
            dispatch_async(dispatch_get_main_queue(), ^{ port_game_menu_show(); });
            continue;
        }
        int role = [self buttonAt:p];
        if (role < 0) {
            const int s = (p.x < self.bounds.size.width / 2) ? 0 : 1;
            if ([self stickHeld:s]) continue;
            role = s ? ROLE_RSTICK : ROLE_LSTICK;
            _stickOrigin[s] = _stickPos[s] = p;
        }
        [_roles setObject:@(role) forKey:t];
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches) {
        NSNumber* r = [_roles objectForKey:t];
        if (!r) continue;
        const CGPoint p = [t locationInView:self];
        const int role = r.intValue;
        if (role == ROLE_LSTICK || role == ROLE_RSTICK) {
            _stickPos[role == ROLE_RSTICK] = p;
        } else {
            /* slide between face buttons and D-pad directions, not onto a stick */
            const int b = [self buttonAt:p];
            if (b >= 0 && b < 8 && role < 8) [_roles setObject:@(b) forKey:t];
        }
    }
    [self publish];
}

- (void)endTouches:(NSSet<UITouch*>*)touches
{
    for (UITouch* t in touches) {
        NSNumber* r = [_roles objectForKey:t];
        if (r && (r.intValue == ROLE_LSTICK || r.intValue == ROLE_RSTICK)) {
            const int s = r.intValue == ROLE_RSTICK;
            _stickOrigin[s] = _stickPos[s] = _stickHome[s];
        }
        [_roles removeObjectForKey:t];
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }

static unsigned stick_axis(CGFloat d, CGFloat radius)
{
    if (!(radius > 0)) return 128;   /* before the first layout */
    CGFloat v = d / radius;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    int a = (int)lround(128.0 + v * 127.5);
    return (unsigned)(a < 0 ? 0 : a > 255 ? 255 : a);
}

- (void)publish
{
    unsigned btn = 0;
    for (UITouch* t in _roles) {
        const int role = [[_roles objectForKey:t] intValue];
        if (role >= 0) btn |= kButtons[role].mask;
    }
    unsigned ax[4];
    for (int s = 0; s < 2; s++) {
        CGFloat dx = _stickPos[s].x - _stickOrigin[s].x, dy = _stickPos[s].y - _stickOrigin[s].y;
        const CGFloat len = sqrt(dx * dx + dy * dy);
        if (len > _stickRadius) {   /* drag the origin along: the stick never sticks at the edge */
            _stickOrigin[s].x = _stickPos[s].x - dx * _stickRadius / len;
            _stickOrigin[s].y = _stickPos[s].y - dy * _stickRadius / len;
            dx = _stickPos[s].x - _stickOrigin[s].x; dy = _stickPos[s].y - _stickOrigin[s].y;
        }
        ax[s * 2]     = stick_axis(dx, _stickRadius);
        ax[s * 2 + 1] = stick_axis(dy, _stickRadius);   /* PS3: 0 = up, 255 = down */
    }
    cellPad_set_touch_state(1, btn, ax[0], ax[1], ax[2], ax[3]);
    _shown = btn;
    [self redraw];
}

- (void)redraw
{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBUTTONS; i++)
        _shape[i].fillColor = [UIColor colorWithWhite:1 alpha:(_shown & kButtons[i].mask) ? 0.55 : 0.18].CGColor;
    for (int s = 0; s < 2; s++) {
        const CGPoint o = _stickOrigin[s], k = _stickPos[s];
        const CGFloat r = _stickRadius, kr = r * 0.45;
        _stickBase[s].path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(o.x - r, o.y - r, 2 * r, 2 * r)].CGPath;
        _stickKnob[s].path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kr, k.y - kr, 2 * kr, 2 * kr)].CGPath;
    }
    [CATransaction commit];
}

- (void)setHidden:(BOOL)hidden
{
    [super setHidden:hidden];
    if (hidden) {
        [_roles removeAllObjects];
        for (int s = 0; s < 2; s++) _stickOrigin[s] = _stickPos[s] = _stickHome[s];
        cellPad_set_touch_state(0, 0, 128, 128, 128, 128);
    } else {
        [self publish];
    }
}
@end

/* ---- shell interface ---------------------------------------------------- */

static PortTouchPad* s_pad;
static int           s_mode = -1;   /* TOUCH_PAD: -1 auto, 0 off, 1 always */
static int           s_gamepads;
static int           s_menu_open;

static void touch_pad_apply(void)
{
    if (!s_pad) return;
    const BOOL hide = s_menu_open || (s_mode != 1 && s_gamepads > 0);
    if (s_pad.hidden != hide) s_pad.hidden = hide;
}

extern "C" void port_touch_pad_attach(void* uiwindow)
{
    const char* e = getenv("TOUCH_PAD");
    s_mode = (e && *e) ? (*e != '0') : -1;
    if (s_mode == 0) { fprintf(stderr, "[ios] touch pad off (TOUCH_PAD=0)\n"); return; }
    UIWindow* w = (__bridge UIWindow*)uiwindow;
    UIView* host = w.rootViewController.view ? w.rootViewController.view : w;
    s_pad = [[PortTouchPad alloc] initWithFrame:host.bounds];
    [host addSubview:s_pad];
    fprintf(stderr, "[ios] touch pad attached (%s)\n", s_mode == 1 ? "always" : "hidden while a gamepad is connected");
}

/* Main thread only. */
extern "C" void port_touch_pad_gamepads(int count)
{
    if (!s_pad || s_mode == 1) return;
    const BOOL was = s_pad.hidden;
    s_gamepads = count;
    touch_pad_apply();
    if (s_pad.hidden != was)
        fprintf(stderr, "[ios] touch pad %s (%d gamepad%s)\n", s_pad.hidden ? "hidden" : "shown", count, count == 1 ? "" : "s");
}

/* Main thread: the in-game menu is up (1) or closed (0). */
extern "C" void port_touch_pad_menu(int open)
{
    s_menu_open = open;
    touch_pad_apply();
}
