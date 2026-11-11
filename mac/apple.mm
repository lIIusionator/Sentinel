// apple.mm -- the Apple-framework calls behind apple.h: CoreGraphics (idle time, lock screen), Foundation (launchd plists), FSEvents (new files).
// Built on a Mac only (mac.mk, -fobjc-arc). Never compiled or run by whoever wrote it: README section 12 says what to look at first.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreServices/CoreServices.h>
#include "apple.h"

namespace sw{

double appleIdleSeconds(){
  const double s=CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,kCGAnyInputEventType);
  return s<0?0:s; }

bool appleScreenLocked(){
  CFDictionaryRef d=CGSessionCopyCurrentDictionary(); if(!d) return false;                        // no session dictionary (not a GUI session): not locked
  auto flag=[&](CFStringRef key,bool absent){ const void* v=CFDictionaryGetValue(d,key); return (v&&CFGetTypeID(v)==CFBooleanGetTypeID())?(bool)CFBooleanGetValue((CFBooleanRef)v):absent; };
  const bool locked=flag(CFSTR("CGSSessionScreenIsLocked"),false)||!flag(CFSTR("kCGSSessionOnConsoleKey"),true);
  CFRelease(d); return locked; }

std::string applePlistProgram(const std::string& plist){
  @autoreleasepool{
    NSString* p=[[NSFileManager defaultManager] stringWithFileSystemRepresentation:plist.c_str() length:plist.size()]; if(!p) return "";
    id d=[NSDictionary dictionaryWithContentsOfFile:p]; if(![d isKindOfClass:[NSDictionary class]]) return "";   // XML and binary plists both load
    id prog=[d objectForKey:@"Program"];
    if(![prog isKindOfClass:[NSString class]]||[(NSString*)prog length]==0){ id a=[d objectForKey:@"ProgramArguments"]; prog=([a isKindOfClass:[NSArray class]]&&[(NSArray*)a count]>0)?[(NSArray*)a objectAtIndex:0]:nil; }
    if(![prog isKindOfClass:[NSString class]]) return "";
    const char* u=[(NSString*)prog UTF8String]; return u?u:""; } }

// ---- FSEvents: files and folders created or renamed in the watched folders ----
static FSEventStreamRef g_stream=nullptr; static dispatch_queue_t g_q=nullptr; static void(*g_onPath)(const std::string&)=nullptr;
static void fsCallback(ConstFSEventStreamRef,void*,size_t n,void* paths,const FSEventStreamEventFlags flags[],const FSEventStreamEventId[]){
  const char* const* p=(const char* const*)paths;                                                  // plain C strings: kFSEventStreamCreateUseCFTypes is not set
  for(size_t i=0;i<n;i++)
    if((flags[i]&(kFSEventStreamEventFlagItemCreated|kFSEventStreamEventFlagItemRenamed))&&g_onPath) g_onPath(p[i]); }
void appleWatchStart(const std::vector<std::string>& dirs,void(*onPath)(const std::string&)){
  if(dirs.empty()||g_stream) return;
  NSMutableArray* a=[NSMutableArray array];
  for(const std::string& d:dirs){ NSString* s=[[NSFileManager defaultManager] stringWithFileSystemRepresentation:d.c_str() length:d.size()]; if(s) [a addObject:s]; }
  if(![a count]) return;
  g_onPath=onPath;
  g_stream=FSEventStreamCreate(kCFAllocatorDefault,fsCallback,nullptr,(__bridge CFArrayRef)a,kFSEventStreamEventIdSinceNow,0.5,
    kFSEventStreamCreateFlagFileEvents|kFSEventStreamCreateFlagNoDefer|kFSEventStreamCreateFlagIgnoreSelf);
  if(!g_stream) return;                                                                            // (the shell notices: no file events, no [FSEVENTS] line)
  g_q=dispatch_queue_create("com.sentinelwatch.fsevents",DISPATCH_QUEUE_SERIAL);
  FSEventStreamSetDispatchQueue(g_stream,g_q);
  if(!FSEventStreamStart(g_stream)){ FSEventStreamInvalidate(g_stream); FSEventStreamRelease(g_stream); g_stream=nullptr; } }
void appleWatchStop(){
  if(!g_stream) return;
  FSEventStreamStop(g_stream); FSEventStreamInvalidate(g_stream); FSEventStreamRelease(g_stream); g_stream=nullptr; g_q=nullptr; }

}
