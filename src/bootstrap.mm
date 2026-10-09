#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include "p2_config.h"
#include <cstdio>
#include <cstring>

// This executable validates the native platform before linking the game.
// It deliberately never calls System::initialize or starts an audio backend.
static int checkMetal(id<MTLDevice> device) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!texture || !queue) { fprintf(stderr, "Metal allocation failed\n"); return 1; }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) { fprintf(stderr, "Metal command creation failed\n"); return 1; }
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "Metal command failed: %s\n", command.error.localizedDescription.UTF8String);
        return 1;
    }
    unsigned char pixels[64] = {};
    [texture getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    for (int i = 0; i < 16; ++i) {
        if (pixels[i*4] != 64 || pixels[i*4+1] != 128 || pixels[i*4+2] != 191 || pixels[i*4+3] != 255) {
            fprintf(stderr, "Metal render/readback mismatch\n"); return 1;
        }
    }
    printf("Metal render/readback passed on %s; audio disabled\n", device.name.UTF8String);
    return 0;
}

@interface P2Bootstrap : NSObject <NSApplicationDelegate, MTKViewDelegate>
@property(strong) NSWindow* window;
@property(strong) id<MTLCommandQueue> queue;
@end

@implementation P2Bootstrap
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.queue = [device newCommandQueue];
    if (!self.queue) { fprintf(stderr, "Metal queue unavailable\n"); [NSApp terminate:nil]; return; }
    NSRect rect = NSMakeRect(0, 0, 960, 640);
    self.window = [[NSWindow alloc] initWithContentRect:rect
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Pikmin 2 — native port bootstrap";
    MTKView* view = [[MTKView alloc] initWithFrame:rect device:device];
    view.delegate = self;
    view.clearColor = MTLClearColorMake(0.025, 0.055, 0.045, 1);
    view.preferredFramesPerSecond = 30;
    self.window.contentView = view;
    NSTextField* label = [NSTextField labelWithString:@"Pikmin 2\nNative Metal bootstrap\n\nAudio disabled · Game engine not yet linked"];
    label.font = [NSFont systemFontOfSize:23 weight:NSFontWeightMedium];
    label.textColor = [NSColor colorWithWhite:0.9 alpha:1];
    label.alignment = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:view.centerYAnchor]]];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }
- (void)drawInMTKView:(MTKView*)view {
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!pass || !drawable) return;
    id<MTLCommandBuffer> command = [self.queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    [command commit];
}
@end

int main(int argc, const char* argv[]) {
    static_assert(P2_AUDIO_ENABLED == 0, "Bootstrap must remain silent");
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "No Metal device available\n"); return 1; }
        if (argc == 2 && !strcmp(argv[1], "--check-metal")) return checkMetal(device);
        if (argc != 1) { fprintf(stderr, "Usage: %s [--check-metal]\n", argv[0]); return 2; }
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        P2Bootstrap* delegate = [P2Bootstrap new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
