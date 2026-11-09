/* iOS / iPadOS first-run screen: import the player's own copy of the game.
 *
 * Shown by ios_shell.mm when Documents/BCES00220 holds no finished import.
 * The player picks a disc image (.iso) or a game folder in the Files picker
 * (on the device, iCloud Drive or a USB drive); an encrypted 1:1 image also
 * needs its .dkey. import/disc_import.c does the work on a background queue
 * and this screen shows progress. The picked file is only read, never moved
 * or changed. When the import is done the screen closes and the game starts. */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

#include "../import/disc_import.h"

@interface PortImportController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, copy) NSString* destRoot;
@property(nonatomic, copy) void (^onDone)(void);
@end

@implementation PortImportController {
    UILabel*        _title;
    UILabel*        _body;
    UIButton*       _pick;
    UIButton*       _cancel;
    UIProgressView* _bar;
    UILabel*        _status;
    UILabel*        _error;
    NSURL*          _source;     /* security-scoped while held (_scoped) */
    BOOL            _scoped;
    NSURL*          _key;        /* a copy in tmp (asCopy:YES) */
    BOOL            _pickingKey;
    volatile int    _stop;
    UIBackgroundTaskIdentifier _bg;
}

static UILabel* make_label(CGFloat size, UIFontWeight weight, UIColor* color)
{
    UILabel* l = [UILabel new];
    l.font = [UIFont systemFontOfSize:size weight:weight];
    l.textColor = color;
    l.numberOfLines = 0;
    l.textAlignment = NSTextAlignmentCenter;
    l.translatesAutoresizingMaskIntoConstraints = NO;
    return l;
}

static UIButton* make_button(NSString* title, BOOL filled)
{
    UIButtonConfiguration* c = filled ? [UIButtonConfiguration filledButtonConfiguration]
                                      : [UIButtonConfiguration grayButtonConfiguration];
    c.title = title;
    c.buttonSize = UIButtonConfigurationSizeLarge;
    c.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;

    _title  = make_label(34, UIFontWeightBold, UIColor.whiteColor);
    _body   = make_label(19, UIFontWeightRegular, [UIColor colorWithWhite:0.85 alpha:1]);
    _status = make_label(15, UIFontWeightRegular, [UIColor colorWithWhite:0.7 alpha:1]);
    _error  = make_label(17, UIFontWeightSemibold, [UIColor colorWithRed:1 green:0.55 blue:0.5 alpha:1]);
    _title.text = @"inFamous";
    _body.text = @"To play, choose your copy of the game.\n\n"
                  "• A disc image (.iso), or\n"
                  "• A game folder (the one with PS3_GAME inside).\n\n"
                  "It needs the European disc, BCES00220. The app copies what it needs "
                  "(about 7.5 GB) and does not change your file.";
    _body.textAlignment = NSTextAlignmentNatural;   /* the bullets read better flush left */
    _pick   = make_button(@"Choose disc image or folder", YES);
    _cancel = make_button(@"Stop", NO);
    _bar    = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _bar.translatesAutoresizingMaskIntoConstraints = NO;
    _cancel.hidden = YES;
    _bar.hidden = YES;

    [_pick addTarget:self action:@selector(pickSource) forControlEvents:UIControlEventTouchUpInside];
    [_cancel addTarget:self action:@selector(stopImport) forControlEvents:UIControlEventTouchUpInside];

    UIStackView* st = [[UIStackView alloc] initWithArrangedSubviews:@[ _title, _body, _pick, _bar, _status, _error, _cancel ]];
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
        [_bar.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_body.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_error.widthAnchor constraintEqualToAnchor:st.widthAnchor],
        [_status.widthAnchor constraintEqualToAnchor:st.widthAnchor],
    ]];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    /* INFAMOUS_IMPORT_TEST=<path> (testing): import that disc image or folder
     * without the picker. A relative path starts in Documents. */
    static BOOL s_tested;
    const char* t = getenv("INFAMOUS_IMPORT_TEST");
    if (s_tested || !t || !*t) return;
    s_tested = YES;
    NSString* p = [NSString stringWithUTF8String:t];
    if (!p.isAbsolutePath)
        p = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
                stringByAppendingPathComponent:p];
    fprintf(stderr, "[import] test switch: %s\n", p.fileSystemRepresentation);
    [self releaseSource];
    _key = nil;
    _source = [NSURL fileURLWithPath:p];
    [self probeAndImport];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersHomeIndicatorAutoHidden { return NO; }

- (void)setBusy:(BOOL)busy
{
    _pick.hidden = busy;
    _cancel.hidden = !busy;
    _bar.hidden = !busy;
    if (busy) { _error.text = nil; _bar.progress = 0; }
    UIApplication.sharedApplication.idleTimerDisabled = busy;   /* no auto-lock mid-import */
}

- (void)showError:(NSString*)msg
{
    [self setBusy:NO];
    _status.text = nil;
    _error.text = msg;
    UIButtonConfiguration* c = _pick.configuration;
    c.title = _pickingKey ? @"Choose disc key (.dkey)" : @"Choose disc image or folder";
    _pick.configuration = c;
}

- (void)releaseSource
{
    if (_source && _scoped) [_source stopAccessingSecurityScopedResource];
    _source = nil;
    _scoped = NO;
}

- (void)pickSource
{
    if (_pickingKey) { [self pickKey]; return; }
    UIDocumentPickerViewController* p =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder, UTTypeData ] asCopy:NO];
    p.delegate = self;
    p.allowsMultipleSelection = NO;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)pickKey
{
    UIDocumentPickerViewController* p =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ] asCopy:YES];
    p.delegate = self;
    p.allowsMultipleSelection = NO;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    NSURL* u = urls.firstObject;
    if (!u) return;
    if (_pickingKey) {
        _key = u;
        [self probeAndImport];
        return;
    }
    [self releaseSource];
    _key = nil;
    _scoped = [u startAccessingSecurityScopedResource];
    if (!_scoped)
        fprintf(stderr, "[import] no security scope for %s (may still be readable)\n", u.path.UTF8String);
    _source = u;
    [self probeAndImport];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {}

- (void)probeAndImport
{
    const std::string src = _source.path.fileSystemRepresentation;
    const std::string key = _key ? _key.path.fileSystemRepresentation : "";
    const std::string dest = self.destRoot.fileSystemRepresentation;
    fprintf(stderr, "[import] source %s%s%s\n", src.c_str(), key.empty() ? "" : ", key ", key.c_str());

    [self setBusy:YES];
    _status.text = @"Checking…";
    _stop = 0;
    _bg = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"import" expirationHandler:^{
        [UIApplication.sharedApplication endBackgroundTask:self->_bg];
        self->_bg = UIBackgroundTaskInvalid;
    }];

    NSURL* url = _source;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        /* Blocks cannot capture arrays: the message lives in a heap buffer. */
        const size_t errlen = 1024;
        NSMutableData* errbuf = [NSMutableData dataWithLength:errlen];
        char* err = (char*)errbuf.mutableBytes;
        __block di_status s = DI_ERR_OPEN;
        /* A coordinated read makes a file provider (iCloud Drive and the
         * like) bring the whole item onto the device first. Local files and
         * USB drives go straight through. */
        NSError* cerr = nil;
        [[NSFileCoordinator new] coordinateReadingItemAtURL:url options:0 error:&cerr byAccessor:^(NSURL* u) {
            const std::string path = u.path.fileSystemRepresentation;
            di_info info;
            s = di_probe(path.c_str(), key.empty() ? NULL : key.c_str(), &info, err, errlen);
            if (s == DI_OK) {
                di_callbacks cb;
                cb.progress = [](void* ctx, double f, const char* what) {
                    PortImportController* me = (__bridge PortImportController*)ctx;
                    NSString* w = [NSString stringWithUTF8String:what ?: ""];
                    dispatch_async(dispatch_get_main_queue(), ^{ [me showProgress:f status:w]; });
                };
                cb.ctx = (__bridge void*)self;
                cb.cancel = &self->_stop;
                s = di_import(path.c_str(), key.empty() ? NULL : key.c_str(), dest.c_str(), &cb, err, errlen);
            }
        }];
        if (cerr) snprintf(err, errlen, "Could not read the chosen item: %s",
                           cerr.localizedDescription.UTF8String ?: "unknown error");
        fprintf(stderr, "[import] %s%s%s\n", di_status_name(s), err[0] ? ": " : "", err);
        NSString* msg = [NSString stringWithUTF8String:err];
        dispatch_async(dispatch_get_main_queue(), ^{ [self finished:s message:msg]; });
    });
}

- (void)showProgress:(double)f status:(NSString*)what
{
    _bar.progress = (float)f;
    _status.text = [NSString stringWithFormat:@"%d%%  %@\nKeep the app open until this finishes.", (int)(f * 100), what];
}

- (void)stopImport
{
    _stop = 1;
    _status.text = @"Stopping…";
}

- (void)finished:(di_status)s message:(NSString*)msg
{
    if (_bg != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_bg];
        _bg = UIBackgroundTaskInvalid;
    }
    if (s == DI_ERR_NEED_KEY || s == DI_ERR_BAD_KEY) {
        _pickingKey = YES;   /* keep the image, ask for its key */
        [self showError:msg];
        return;
    }
    _pickingKey = NO;
    [self releaseSource];
    if (s != DI_OK) { [self showError:msg]; return; }
    [self setBusy:NO];
    _pick.hidden = YES;
    _status.text = @"Done. Starting the game…";
    void (^done)(void) = self.onDone;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self dismissViewControllerAnimated:YES completion:^{ if (done) done(); }];
    });
}

@end

/* Main thread. Shows the import screen over the window; calls done once a
 * finished import is in dest_root. */
extern "C" void port_ios_import_show(void* uiwindow, const char* dest_root, void (^done)(void))
{
    UIWindow* win = (__bridge UIWindow*)uiwindow;
    PortImportController* c = [PortImportController new];
    c.destRoot = [NSString stringWithUTF8String:dest_root];
    c.onDone = done;
    c.modalPresentationStyle = UIModalPresentationFullScreen;
    UIViewController* root = win.rootViewController;
    while (root.presentedViewController) root = root.presentedViewController;
    [root presentViewController:c animated:NO completion:nil];
}
