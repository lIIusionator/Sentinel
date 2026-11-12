// app.mm -- the AppKit shell of the Sentinel Watch Mac app: menu-bar item, dashboard window (a WKWebView showing ui/dashboard.html), toast, AFK screen (AVFoundation).
// Every decision is made in core.cpp (portable, tested on Linux against a fake); this file only shows the results. Built on a Mac only (mac.mk, -fobjc-arc, macOS 11+).
// Compiled by GitHub's macOS runner, never run by whoever wrote it: README section 12 says what to look at first.
// Rules for this file: no property syntax for names that are C++ keywords (img.template); no NSColor.CGColor (macOS 14+, use CG()); no private API.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#include <string.h>
#include <unistd.h>
#include "core.h"
#include "apple.h"

static NSString* g_res=nil;                                                                  // <app>/Contents/Resources
static id<NSObject> g_activity=nil;                                                          // keeps App Nap off: the watchers and the idle check must run on time

static NSString* toNS(const std::string& s){ NSString* r=[NSString stringWithUTF8String:sw::utf8Clean(s).c_str()]; return r?r:@""; }
static std::string toStd(NSString* s){ const char* u=s?[s UTF8String]:nullptr; return u?u:""; }
static NSColor* RGB(int r,int g,int b,double a=1){ return [NSColor colorWithSRGBRed:r/255.0 green:g/255.0 blue:b/255.0 alpha:a]; }
static CGColorRef CG(int r,int g,int b,double a=1){ return (CGColorRef)CFAutorelease(CGColorCreateSRGB(r/255.0,g/255.0,b/255.0,a)); }   // NSColor.CGColor is macOS 14+
static NSParagraphStyle* centered(){ static NSParagraphStyle* p=nil; if(!p){ NSMutableParagraphStyle* m=[[NSMutableParagraphStyle alloc] init]; [m setAlignment:NSTextAlignmentCenter]; p=m; } return p; }
static void activate(){
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  [NSApp activateIgnoringOtherApps:YES];                                                     // macOS 14 prefers -activate; this one still works everywhere
#pragma clang diagnostic pop
}
// "plain **bold** plain": the quote lines use ** for bold (the Windows app does the same)
static NSAttributedString* rich(NSString* raw,NSFont* reg,NSFont* bold,NSColor* col){
  NSMutableAttributedString* out=[[NSMutableAttributedString alloc] init];
  NSArray<NSString*>* parts=[raw componentsSeparatedByString:@"**"];
  for(NSUInteger i=0;i<[parts count];i++){ NSString* p=[parts objectAtIndex:i]; if(![p length]) continue;
    [out appendAttributedString:[[NSAttributedString alloc] initWithString:p attributes:@{NSFontAttributeName:(i%2?bold:reg),NSForegroundColorAttributeName:col,NSParagraphStyleAttributeName:centered()}]]; }
  return out; }
static std::string displayStr(){ NSScreen* s=[NSScreen mainScreen]; if(!s) return ""; const NSRect f=[s frame]; const CGFloat k=[s backingScaleFactor];
  return std::to_string((int)(f.size.width*k+0.5))+" x "+std::to_string((int)(f.size.height*k+0.5)); }

// ================= toast =================
static NSPanel* g_toast=nil; static unsigned g_toastGen=0;
static NSTextField* label(NSString* s,NSFont* f,NSColor* c,NSRect r,NSInteger lines){
  NSTextField* t=[NSTextField wrappingLabelWithString:s]; [t setFont:f]; [t setTextColor:c]; [t setFrame:r]; [t setMaximumNumberOfLines:lines]; [t setLineBreakMode:NSLineBreakByCharWrapping]; [t setSelectable:NO]; return t; }
static void showToast(NSString* title,NSString* text,bool red,bool beep){
  if(beep) NSBeep();
  if(g_toast){ [g_toast orderOut:nil]; g_toast=nil; }
  NSScreen* sc=[NSScreen mainScreen]; if(!sc) return;
  const CGFloat W=380, pad=14, th=18;
  NSFont* tf=[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]; NSFont* xf=[NSFont systemFontOfSize:12];
  CGFloat xh=ceil([text boundingRectWithSize:NSMakeSize(W-2*pad,1000) options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading attributes:@{NSFontAttributeName:xf}].size.height)+2;
  if(xh>96) xh=96;
  const CGFloat H=pad+th+6+xh+pad;
  NSPanel* p=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,W,H) styleMask:NSWindowStyleMaskBorderless|NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
  [p setLevel:NSStatusWindowLevel]; [p setOpaque:NO]; [p setBackgroundColor:[NSColor clearColor]]; [p setHasShadow:YES]; [p setReleasedWhenClosed:NO]; [p setIgnoresMouseEvents:YES];
  [p setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces|NSWindowCollectionBehaviorStationary|NSWindowCollectionBehaviorFullScreenAuxiliary];
  NSView* box=[[NSView alloc] initWithFrame:NSMakeRect(0,0,W,H)]; [box setWantsLayer:YES];
  CALayer* bl=[box layer]; [bl setBackgroundColor:CG(20,26,46,0.97)]; [bl setCornerRadius:10]; [bl setBorderWidth:1]; [bl setBorderColor:red?CG(248,116,140,0.9):CG(54,66,108)];
  [box addSubview:label(title,tf,red?RGB(248,116,140):RGB(120,196,255),NSMakeRect(pad,H-pad-th,W-2*pad,th),1)];
  [box addSubview:label(text,xf,RGB(200,206,226),NSMakeRect(pad,pad,W-2*pad,xh),6)];
  [p setContentView:box];
  const NSRect vf=[sc visibleFrame]; [p setFrameOrigin:NSMakePoint(NSMaxX(vf)-W-16,NSMaxY(vf)-H-16)];
  [p orderFrontRegardless]; g_toast=p;
  const unsigned gen=++g_toastGen;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(7*NSEC_PER_SEC)),dispatch_get_main_queue(),^{ if(g_toastGen==gen&&g_toast){ [g_toast orderOut:nil]; g_toast=nil; } });
}

// ================= dashboard window =================
@interface SWDash : NSObject <NSWindowDelegate,WKScriptMessageHandler,WKNavigationDelegate>
@property(strong) NSWindow* win; @property(strong) WKWebView* web; @property(strong) NSTimer* timer; @property(assign) BOOL ready; @property(assign) unsigned lastRev;
- (void)show; - (void)push:(BOOL)force; - (void)saveSize; - (void)teardown;
@end
@implementation SWDash
- (void)show{
  if(self.win){ if([self.win isMiniaturized]) [self.win deminiaturize:nil]; [self.win makeKeyAndOrderFront:nil]; activate(); return; }
  NSWindow* w=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,sw::g_dashW,sw::g_dashH)
    styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  [w setTitle:@"Sentinel Watch"]; [w setTitlebarAppearsTransparent:YES]; [w setTitleVisibility:NSWindowTitleHidden];
  [w setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]]; [w setBackgroundColor:RGB(10,14,26)];
  [w setContentMinSize:NSMakeSize(sw::DASH_MINW,sw::DASH_MINH)]; [w setReleasedWhenClosed:NO]; [w setDelegate:self]; [w center];
  WKWebViewConfiguration* cfg=[[WKWebViewConfiguration alloc] init];
  [[cfg userContentController] addScriptMessageHandler:self name:@"sw"];                     // the page talks to us through window.webkit.messageHandlers.sw
  WKWebView* v=[[WKWebView alloc] initWithFrame:[[w contentView] bounds] configuration:cfg];
  [v setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable]; [v setNavigationDelegate:self];
  [[w contentView] addSubview:v];
  NSURL* page=[NSURL fileURLWithPath:[g_res stringByAppendingPathComponent:@"ui/dashboard.html"]];
  [v loadFileURL:page allowingReadAccessToURL:[page URLByDeletingLastPathComponent]];
  self.win=w; self.web=v; self.ready=NO; self.lastRev=0;
  __weak SWDash* ws=self;
  self.timer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* t){ (void)t; [ws push:NO]; }];
  [self.timer setTolerance:0.3];
  [w makeKeyAndOrderFront:nil]; activate(); }
- (void)push:(BOOL)force{                                                                    // the page redraws only when something it shows has changed
  if(!self.web||!self.ready) return;
  const unsigned rev=sw::g_rev.load(); if(!force&&rev==self.lastRev) return;
  self.lastRev=rev;
  [self.web evaluateJavaScript:[NSString stringWithFormat:@"swState(%@)",toNS(sw::stateJson(displayStr()))] completionHandler:nil]; }
- (void)userContentController:(WKUserContentController*)uc didReceiveScriptMessage:(WKScriptMessage*)m{
  (void)uc; if(![m.body isKindOfClass:[NSDictionary class]]) return;
  NSDictionary* d=(NSDictionary*)m.body; NSString* cmd=[d objectForKey:@"cmd"]; if(![cmd isKindOfClass:[NSString class]]) return;
  if([cmd isEqualToString:@"ready"]){ self.ready=YES; [self push:YES]; }
  else if([cmd isEqualToString:@"toggle"]){ NSString* k=[d objectForKey:@"key"]; if([k isKindOfClass:[NSString class]]){ sw::cmdToggle(toStd(k)); [self push:YES]; } }
  else if([cmd isEqualToString:@"scan"]) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{ sw::cmdScan(); });
  else if([cmd isEqualToString:@"reveal"]){ NSString* p=[d objectForKey:@"path"];                       // only a path that belongs to an event in the feed
    if([p isKindOfClass:[NSString class]]&&sw::cmdRevealOk(toStd(p))){ NSFileManager* fm=[NSFileManager defaultManager];
      NSString* at=[fm fileExistsAtPath:p]?p:[p stringByDeletingLastPathComponent];                  // a program that is already gone: its folder
      if([fm fileExistsAtPath:at]) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:at]]]; } }
  else if([cmd isEqualToString:@"social"]){ NSNumber* i=[d objectForKey:@"i"];                          // the page sends an index, never a URL
    static NSString* const urls[2]={@"https://www.youtube.com/@lIIusionator",@"https://www.roblox.com/users/10679736474/profile"};
    if(![i isKindOfClass:[NSNumber class]]) return;
    const int n=[i intValue];
    if(n>=0&&n<2) [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:urls[n]] configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
    else if(n==2){ NSPasteboard* pb=[NSPasteboard generalPasteboard]; [pb clearContents]; [pb setString:@"liiusionator" forType:NSPasteboardTypeString]; } } }
- (void)webView:(WKWebView*)w decidePolicyForNavigationAction:(WKNavigationAction*)a decisionHandler:(void(^)(WKNavigationActionPolicy))h{
  (void)w; h([[a.request URL] isFileURL]?WKNavigationActionPolicyAllow:WKNavigationActionPolicyCancel); }   // nothing but our own page, ever
- (void)saveSize{ if(!self.win) return; const NSSize s=[self.win contentRectForFrameRect:[self.win frame]].size;
  sw::g_dashW=(int)s.width; sw::g_dashH=(int)s.height; sw::saveSettings(); }
- (void)teardown{                                                                            // the page is destroyed with the window: a closed dashboard costs nothing
  if(!self.win) return;
  NSWindow* w=self.win; WKWebView* v=self.web;
  [self saveSize]; [self.timer invalidate]; self.timer=nil;
  [[[v configuration] userContentController] removeScriptMessageHandlerForName:@"sw"];       // (it holds us: this breaks the cycle)
  [v setNavigationDelegate:nil]; [v stopLoading];
  self.win=nil; self.web=nil; self.ready=NO;
  dispatch_async(dispatch_get_main_queue(),^{ (void)w; (void)v; }); }                        // both live until AppKit is done with them
- (void)windowWillClose:(NSNotification*)n{ (void)n; [self teardown]; }
@end

// ================= AFK screen =================
@interface SWAfkWindow : NSWindow @end
@implementation SWAfkWindow
- (BOOL)canBecomeKeyWindow{ return YES; }                                                    // a borderless window is not key by default: the first key press must land here, not in another app
- (BOOL)canBecomeMainWindow{ return YES; }
- (void)keyDown:(NSEvent*)e{ (void)e; }                                                      // swallowed (no beep)
- (void)mouseDown:(NSEvent*)e{ (void)e; }
@end

static CATextLayer* textLayer(CGFloat scale){ CATextLayer* t=[CATextLayer layer]; [t setContentsScale:scale]; [t setWrapped:YES]; [t setAlignmentMode:kCAAlignmentCenter]; return t; }

// shortcut: the clock + quote HUD is on the main display only, no starfield, no preload (black for the first ~0.3 s); upgrade: a paused AVPlayer kept ready, an overlay per display
@interface SWAfk : NSObject
@property(readonly) BOOL up;
- (void)showWithQuote:(NSString*)q; - (void)setQuote:(NSString*)q; - (void)hide;
@end
@implementation SWAfk{
  NSMutableArray<NSWindow*>* _wins; AVQueuePlayer* _player; AVPlayerLooper* _looper; NSTimer* _clockTimer; NSDateFormatter* _fmt; NSFont* _clockFont;
  CATextLayer* _clock; CATextLayer* _quote; CGFloat _scale,_qTop,_qBottom,_qW,_W; BOOL _cursorHidden; }
- (BOOL)up{ return _wins!=nil; }
- (void)buildTextOn:(CALayer*)root size:(NSSize)sz scale:(CGFloat)sc{
  _scale=MAX(0.6,MIN(2.0,sz.height/1080.0)); _W=sz.width; _clockFont=[NSFont monospacedDigitSystemFontOfSize:round(46*_scale) weight:NSFontWeightBold];
  const CGFloat clockH=ceil(60*_scale), clockY=sz.height*0.86-clockH, hintH=ceil(20*_scale), hintY=sz.height*0.05;
  _clock=textLayer(sc); [_clock setFrame:CGRectMake(0,clockY,sz.width,clockH)]; [root addSublayer:_clock];
  CATextLayer* hint=textLayer(sc); [hint setFrame:CGRectMake(0,hintY,sz.width,hintH)];
  [hint setString:[[NSAttributedString alloc] initWithString:@"AFK  ·  move or press a key to resume"
    attributes:@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:round(12*_scale) weight:NSFontWeightRegular],NSForegroundColorAttributeName:RGB(255,255,255,0.55),NSParagraphStyleAttributeName:centered()}]];
  [root addSublayer:hint];
  _quote=textLayer(sc); [root addSublayer:_quote];
  _qTop=clockY-sz.height*0.05; _qBottom=hintY+hintH+sz.height*0.06; _qW=MIN(sz.width*0.62,1100*_scale); }
- (void)tickClock{ if(!_clock) return;
  [_clock setString:[[NSAttributedString alloc] initWithString:[_fmt stringFromDate:[NSDate date]]
    attributes:@{NSFontAttributeName:_clockFont,NSForegroundColorAttributeName:RGB(255,255,255,0.92),NSParagraphStyleAttributeName:centered()}]]; }
- (void)setQuote:(NSString*)q{                                                               // the biggest type that fits between the clock and the hint line
  if(!_quote) return;
  const CGFloat availH=_qTop-_qBottom; if(availH<20){ [_quote setString:@""]; return; }
  NSAttributedString* best=nil; CGFloat bestH=0;
  for(CGFloat s=round(40*_scale);s>=12;s-=2){
    NSAttributedString* a=rich(q,[NSFont systemFontOfSize:s weight:NSFontWeightRegular],[NSFont systemFontOfSize:s weight:NSFontWeightBold],RGB(255,255,255,0.88));
    const CGFloat h=ceil([a boundingRectWithSize:NSMakeSize(_qW,10000) options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading].size.height)+4;
    best=a; bestH=h; if(h<=availH) break; }
  const CGFloat h=MIN(bestH,availH);
  [_quote setFrame:CGRectMake((_W-_qW)/2,_qBottom+(availH-h)/2,_qW,h)]; [_quote setString:best]; }
- (void)showWithQuote:(NSString*)q{
  if(_wins) return;
  NSArray<NSScreen*>* screens=[NSScreen screens]; if(![screens count]) return;
  NSString* clip=toNS(sw::pickAfkVideo(toStd(g_res)));
  if([clip length]){
    _player=[[AVQueuePlayer alloc] init];
    _looper=[AVPlayerLooper playerLooperWithPlayer:_player templateItem:[AVPlayerItem playerItemWithURL:[NSURL fileURLWithPath:clip]]];
    [_player play]; }
  sw::logLine("[AFK] screen up on "+std::to_string([screens count])+" display(s); video: "+([clip length]?toStd([clip lastPathComponent]):std::string("none")));
  _fmt=[[NSDateFormatter alloc] init]; [_fmt setLocale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]]; [_fmt setDateFormat:@"HH:mm:ss"];
  _wins=[NSMutableArray array];
  for(NSUInteger i=0;i<[screens count];i++){
    NSScreen* s=[screens objectAtIndex:i]; const NSRect f=[s frame];
    SWAfkWindow* w=[[SWAfkWindow alloc] initWithContentRect:NSMakeRect(0,0,f.size.width,f.size.height) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    [w setFrame:f display:NO]; [w setLevel:NSScreenSaverWindowLevel]; [w setOpaque:YES]; [w setBackgroundColor:[NSColor blackColor]]; [w setHasShadow:NO]; [w setReleasedWhenClosed:NO];
    [w setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces|NSWindowCollectionBehaviorStationary|NSWindowCollectionBehaviorFullScreenAuxiliary|NSWindowCollectionBehaviorIgnoresCycle];
    NSView* v=[[NSView alloc] initWithFrame:NSMakeRect(0,0,f.size.width,f.size.height)];
    CALayer* root=[CALayer layer]; [root setFrame:[v bounds]]; [root setBackgroundColor:CG(0,0,0)];
    [v setLayer:root]; [v setWantsLayer:YES];                                                // layer-hosting (layer set first): this layer tree is ours
    if(_player){ AVPlayerLayer* pl=[AVPlayerLayer playerLayerWithPlayer:_player]; [pl setFrame:[root bounds]]; [pl setVideoGravity:AVLayerVideoGravityResizeAspectFill]; [root addSublayer:pl]; }
    CAGradientLayer* gr=[CAGradientLayer layer]; [gr setFrame:CGRectMake(0,0,f.size.width,f.size.height*0.5)];   // darker toward the bottom edge (macOS layers: y grows upward)
    [gr setColors:@[(__bridge id)CG(0,0,0,0.65),(__bridge id)CG(0,0,0,0)]]; [root addSublayer:gr];
    if(i==0) [self buildTextOn:root size:f.size scale:[s backingScaleFactor]];
    [w setContentView:v]; [_wins addObject:w]; }
  [self setQuote:q]; [self tickClock];
  __weak SWAfk* ws=self;
  _clockTimer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* t){ (void)t; [ws tickClock]; }];
  activate();
  for(NSUInteger i=0;i<[_wins count];i++){ NSWindow* w=[_wins objectAtIndex:i]; if(i==0) [w makeKeyAndOrderFront:nil]; else [w orderFrontRegardless]; }
  if(!_cursorHidden){ [NSCursor hide]; _cursorHidden=YES; }
  if(_looper) dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{                // the first real-Mac evidence about the video: events.log
    SWAfk* me=ws; if(!me||!me->_looper) return;
    NSString* err=[[me->_looper error] localizedDescription];
    sw::logLine("[AFKVID] 2 s in: looper status "+std::to_string((int)[me->_looper status])+" (1 ready, 2 failed), player "+std::to_string((int)[me->_player timeControlStatus])+" (2 playing)"+(err?" -- "+toStd(err):std::string())); }); }
- (void)hide{
  if(!_wins) return;
  [_clockTimer invalidate]; _clockTimer=nil;
  [_looper disableLooping]; _looper=nil; [_player pause]; _player=nil;
  for(NSWindow* w in _wins) [w orderOut:nil];
  _wins=nil; _clock=nil; _quote=nil;
  if(_cursorHidden){ [NSCursor unhide]; _cursorHidden=NO; }
  sw::logLine("[AFK] screen down"); }
@end

// ================= application =================
@interface SWApp : NSObject <NSApplicationDelegate,NSMenuDelegate>
- (instancetype)initWithBackground:(BOOL)bg;
@end
@implementation SWApp{ NSStatusItem* _item; NSMenuItem* _notify; SWDash* _dash; SWAfk* _afk; NSTimer* _tick; BOOL _background; }
- (instancetype)initWithBackground:(BOOL)bg{ if((self=[super init])) _background=bg; return self; }
- (void)applicationDidFinishLaunching:(NSNotification*)n{
  (void)n;
  g_activity=[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"Sentinel Watch is watching this Mac"];
  sw::g_toastHook=[](const std::string& t,const std::string& x,bool red,bool beep){ NSString* a=toNS(t); NSString* b=toNS(x); dispatch_async(dispatch_get_main_queue(),^{ showToast(a,b,red,beep); }); };
  sw::appInit(toStd(g_res),_background);
  sw::engineStart();
  { const std::vector<std::string> dirs=sw::watchedDirs(); sw::logLine("[FSEVENTS] watching "+std::to_string(dirs.size())+" folder(s)"); sw::appleWatchStart(dirs,&sw::fileEvent); }
  _dash=[[SWDash alloc] init]; _afk=[[SWAfk alloc] init];
  _item=[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
  NSImage* img=[NSImage imageWithSystemSymbolName:@"eye" accessibilityDescription:@"Sentinel Watch"];
  if(img){ [img setTemplate:YES]; [[_item button] setImage:img]; } else [[_item button] setTitle:@"SW"];
  NSMenu* m=[[NSMenu alloc] init]; [m setDelegate:self];
  [[m addItemWithTitle:@"Open dashboard" action:@selector(openDash:) keyEquivalent:@""] setTarget:self];
  [[m addItemWithTitle:@"Quick scan" action:@selector(quickScan:) keyEquivalent:@""] setTarget:self];
  _notify=[m addItemWithTitle:@"Notifications" action:@selector(toggleNotify:) keyEquivalent:@""]; [_notify setTarget:self];
  [m addItem:[NSMenuItem separatorItem]];
  [[m addItemWithTitle:@"Quit Sentinel Watch" action:@selector(quit:) keyEquivalent:@""] setTarget:self];
  [_item setMenu:m];
  [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(openDash:) name:@"com.sentinelwatch.open" object:nil
    suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];                  // a second copy started from a terminal asks this one to show itself
  [self armTick:1.0];
  if(!_background) [_dash show]; }                                                           // started at login (--background): menu bar only
- (void)armTick:(NSTimeInterval)s{
  [_tick invalidate];
  __weak SWApp* ws=self;
  _tick=[NSTimer scheduledTimerWithTimeInterval:s repeats:YES block:^(NSTimer* t){ (void)t; [ws tick]; }];
  [_tick setTolerance:s*0.1]; }
- (void)tick{                                                                                // idle check: once a second, four times a second while the AFK screen is up
  const bool locked=sw::g_sys->screenLocked();
  if(!sw::quoteTick(locked?0:sw::g_sys->idleSeconds())) return;
  const sw::Afk a=sw::afkState();
  if(a.on&&![_afk up]){ [_afk showWithQuote:toNS(a.quote)]; [self armTick:0.25]; }
  else if(!a.on&&[_afk up]){ [_afk hide]; [self armTick:1.0]; }
  else if(a.on) [_afk setQuote:toNS(a.quote)]; }
- (void)openDash:(id)sender{ (void)sender; [_dash show]; }
- (void)quickScan:(id)sender{ (void)sender; dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{ sw::cmdScan(); }); }
- (void)toggleNotify:(id)sender{ (void)sender; sw::cmdToggle("notify"); }
- (void)quit:(id)sender{ (void)sender; [NSApp terminate:nil]; }
- (void)menuNeedsUpdate:(NSMenu*)menu{ (void)menu; [_notify setState:sw::g_tNotify.load()?NSControlStateValueOn:NSControlStateValueOff]; }
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)flag{ (void)app; (void)flag; [_dash show]; return YES; }   // double-click in Finder while it runs hidden
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app{ (void)app; return NO; }
- (void)applicationWillTerminate:(NSNotification*)n{
  (void)n; [_afk hide]; [_dash teardown]; sw::appleWatchStop(); sw::engineStop(); sw::saveSettings(); sw::releaseInstanceLock();
  fflush(nullptr); _exit(0); }                                                               // skips the static destructors: a strings-scan thread may still be reading a file
@end

static SWApp* g_app=nil;                                                                     // NSApplication.delegate is weak
int main(int argc,const char* argv[]){
  @autoreleasepool{
    bool background=false; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--background")) background=true;
    NSBundle* b=[NSBundle mainBundle]; g_res=[b resourcePath]; sw::g_selfExe=toStd([b executablePath]);
    sw::g_sys=sw::newMacSys();
    if(!sw::acquireInstanceLock()){                                                          // already running: show that one (it may be hidden in the menu bar)
      if(!background) [[NSDistributedNotificationCenter defaultCenter] postNotificationName:@"com.sentinelwatch.open" object:nil userInfo:nil deliverImmediately:YES];
      return 0; }
    NSApplication* app=[NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];                        // no Dock icon, no menu bar of its own (Info.plist says LSUIElement as well)
    g_app=[[SWApp alloc] initWithBackground:background]; [app setDelegate:g_app];
    [app run]; }
  return 0; }
