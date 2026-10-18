#include "host/window.h"

#include "gpu/renderer.h"
#include "host/input.h"

#include "engine/log.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <thread>

@interface UtsuroView : NSView
@end

static bool g_captured = false;
static void SetMouseCaptured(bool captured) {
    if (captured == g_captured) return;
    g_captured = captured;
    CGAssociateMouseAndMouseCursorPosition(captured ? NO : YES);
    if (captured) [NSCursor hide]; else [NSCursor unhide];
}

@implementation UtsuroView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 0x35 && g_captured) SetMouseCaptured(false);   // Esc also gives the mouse back
    if (!event.isARepeat) utsuro::input::KeyEvent(event.keyCode, true);
}
- (void)keyUp:(NSEvent*)event { utsuro::input::KeyEvent(event.keyCode, false); }
- (void)flagsChanged:(NSEvent*)event {
    utsuro::input::KeyEvent(0x38, (event.modifierFlags & NSEventModifierFlagShift) != 0);
}
- (void)mouseDown:(NSEvent*)event { SetMouseCaptured(true); utsuro::input::MouseButton(0, true); }
- (void)mouseUp:(NSEvent*)event { utsuro::input::MouseButton(0, false); }
- (void)rightMouseDown:(NSEvent*)event { utsuro::input::MouseButton(1, true); }
- (void)rightMouseUp:(NSEvent*)event { utsuro::input::MouseButton(1, false); }
- (void)mouseMoved:(NSEvent*)event { utsuro::input::MouseMoved((float)event.deltaX, (float)event.deltaY); }
- (void)mouseDragged:(NSEvent*)event { utsuro::input::MouseMoved((float)event.deltaX, (float)event.deltaY); }
@end

@interface UtsuroDelegate : NSObject <NSApplicationDelegate>
@end
@implementation UtsuroDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }
@end

namespace utsuro::host {

int RunWindowed(std::function<int()> run_title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        static UtsuroDelegate* delegate = [UtsuroDelegate new];
        [NSApp setDelegate:delegate];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        {
            NSMenu* bar = [NSMenu new];
            NSMenuItem* app_item = [NSMenuItem new];
            [bar addItem:app_item];
            NSMenu* app_menu = [NSMenu new];
            [app_menu addItemWithTitle:@"Quit Utsuro" action:@selector(terminate:) keyEquivalent:@"q"];
            app_item.submenu = app_menu;
            [NSApp setMainMenu:bar];
        }
        const NSRect frame = NSMakeRect(0, 0, 1280, 720);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"Utsuro - Minecraft: Xbox One Edition";
        UtsuroView* view = [[UtsuroView alloc] initWithFrame:frame];
        view.wantsLayer = YES;
        window.contentView = view;
        window.acceptsMouseMovedEvents = YES;
        [window makeFirstResponder:view];
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        gpu::Init();
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        layer.contentsScale = window.backingScaleFactor;
        layer.drawableSize = CGSizeMake(1920, 1080);
        gpu::SetLayer((__bridge void*)layer);

        auto result = std::make_shared<std::atomic<int>>(0);
        std::thread([run_title, result] {
            result->store(run_title());
            dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        }).detach();
        [NSApp run];
        return result->load();
    }
}

} // namespace utsuro::host
