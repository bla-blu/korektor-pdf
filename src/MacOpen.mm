#include "MacOpen.h"

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

static std::vector<std::string> gOpened;

// -[NSApplicationDelegate application:openURLs:] dopisywana do delegata, którego tworzy GLFW.
static void kpdfOpenURLs(id, SEL, NSApplication*, NSArray<NSURL*>* urls) {
    for (NSURL* u in urls)
        if (u.isFileURL && u.path) gOpened.emplace_back(u.path.UTF8String);
    // obudź pętlę programu, jeśli czeka na zdarzenia
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSMakePoint(0, 0)
                                  modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                        subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:NO];
}

void macInstallOpenHandler() {
    Class cls = objc_getClass("GLFWApplicationDelegate");   // klasa z GLFW 3.x (cocoa_init.m)
    if (cls) class_addMethod(cls, @selector(application:openURLs:), (IMP)kpdfOpenURLs, "v@:@@");
}

std::vector<std::string> macTakeOpenedFiles() {
    std::vector<std::string> out;
    out.swap(gOpened);
    return out;
}
