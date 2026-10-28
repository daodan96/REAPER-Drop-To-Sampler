// macOS: the TCP/MCP window is a SWELL NSView. Attach() swaps the view's class for a
// generated subclass that overrides the NSDraggingDestination methods, so file drags
// reach DragHandler instead of REAPER (the same takeover as RegisterDragDrop on Windows).
// Drags without files are passed on to REAPER unchanged.

#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include "platform/platform.h"

#include <cmath>
#include <string>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // NSFilenamesPboardType: what SWELL drags use

static const char* const kSubclassPrefix = "DropToSampler_";
static char kHandlerKey;

static DragHandler* HandlerFor(id view)
{
    NSValue* v = objc_getAssociatedObject(view, &kHandlerKey);
    return v ? (DragHandler*)[v pointerValue] : nullptr;
}

static Modifiers CurrentModifiers()
{
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    Modifiers m;
    m.shift = (flags & NSEventModifierFlagShift) != 0;
    m.ctrl = (flags & NSEventModifierFlagCommand) != 0; // REAPER maps Ctrl to Cmd on macOS
    m.alt = (flags & NSEventModifierFlagOption) != 0;
    return m;
}

// Screen coordinates the way SWELL reports them (GetCursorPos, GetThingFromPoint)
static void ScreenPoint(NSView* view, id<NSDraggingInfo> info, int* x, int* y)
{
    NSPoint p = [info draggingLocation];
    NSRect r = [[view window] convertRectToScreen:NSMakeRect(p.x, p.y, 0, 0)];
    *x = (int)floor(r.origin.x + 0.5);
    *y = (int)floor(r.origin.y + 0.5);
}

static NSDictionary* FileUrlOptions()
{
    return @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES };
}

static bool HasFiles(id<NSDraggingInfo> info)
{
    NSPasteboard* pb = [info draggingPasteboard];
    return [pb canReadObjectForClasses:@[[NSURL class]] options:FileUrlOptions()]
        || [[pb types] containsObject:NSFilenamesPboardType];
}

static std::vector<std::string> DroppedFiles(id<NSDraggingInfo> info)
{
    std::vector<std::string> files;
    NSPasteboard* pb = [info draggingPasteboard];
    NSArray* urls = [pb readObjectsForClasses:@[[NSURL class]] options:FileUrlOptions()];
    for (NSURL* url in urls)
    {
        const char* path = [[url path] UTF8String];
        if (path && path[0]) files.push_back(path);
    }
    if (files.empty())
    {
        NSArray* names = [pb propertyListForType:NSFilenamesPboardType];
        if ([names isKindOfClass:[NSArray class]])
        {
            for (id name in names)
            {
                const char* path = [name isKindOfClass:[NSString class]] ? [(NSString*)name UTF8String] : nullptr;
                if (path && path[0]) files.push_back(path);
            }
        }
    }
    return files;
}

// Calls the original (SWELL) implementation, if the original class has one
template <typename R>
static R CallSuper(id self, SEL sel, id arg, R fallback)
{
    Class base = class_getSuperclass(object_getClass(self));
    if (!class_respondsToSelector(base, sel)) return fallback;
    struct objc_super sup;
    sup.receiver = self;
    sup.super_class = base;
    return ((R(*)(struct objc_super*, SEL, id))objc_msgSendSuper)(&sup, sel, arg);
}

static NSDragOperation Hook_draggingEntered(id self, SEL sel, id<NSDraggingInfo> info)
{
    DragHandler* handler = HandlerFor(self);
    if (!handler || !HasFiles(info))
        return CallSuper<NSDragOperation>(self, sel, info, NSDragOperationNone);
    int x, y;
    ScreenPoint(self, info, &x, &y);
    handler->OnDragEnter((int)DroppedFiles(info).size(), x, y, CurrentModifiers());
    return NSDragOperationCopy;
}

static NSDragOperation Hook_draggingUpdated(id self, SEL sel, id<NSDraggingInfo> info)
{
    DragHandler* handler = HandlerFor(self);
    if (!handler || !HasFiles(info))
        return CallSuper<NSDragOperation>(self, sel, info, NSDragOperationNone);
    int x, y;
    ScreenPoint(self, info, &x, &y);
    handler->OnDragOver(x, y, CurrentModifiers());
    return NSDragOperationCopy;
}

static void Hook_draggingExited(id self, SEL sel, id<NSDraggingInfo> info)
{
    DragHandler* handler = HandlerFor(self);
    if (handler && info && HasFiles(info))
    {
        handler->OnDragLeave();
        return;
    }
    Class base = class_getSuperclass(object_getClass(self));
    if (!class_respondsToSelector(base, sel)) return;
    struct objc_super sup;
    sup.receiver = self;
    sup.super_class = base;
    ((void (*)(struct objc_super*, SEL, id))objc_msgSendSuper)(&sup, sel, info);
}

static BOOL Hook_performDragOperation(id self, SEL sel, id<NSDraggingInfo> info)
{
    DragHandler* handler = HandlerFor(self);
    std::vector<std::string> files;
    if (handler)
        files = DroppedFiles(info);
    if (files.empty())
        return CallSuper<BOOL>(self, sel, info, NO);
    int x, y;
    ScreenPoint(self, info, &x, &y);
    handler->OnDrop(files, x, y, CurrentModifiers());
    return YES;
}

// Keep updating while the mouse rests, so modifier changes show up immediately
static BOOL Hook_wantsPeriodicDraggingUpdates(id self, SEL sel)
{
    (void)self; (void)sel;
    return YES;
}

static Class HookedSubclassOf(Class base)
{
    std::string name = std::string(kSubclassPrefix) + class_getName(base);
    Class sub = objc_getClass(name.c_str());
    if (sub) return sub;

    sub = objc_allocateClassPair(base, name.c_str(), 0);
    if (!sub) return nil;

    const std::string op = @encode(NSDragOperation);
    const std::string boolean = @encode(BOOL);
    class_addMethod(sub, @selector(draggingEntered:), (IMP)Hook_draggingEntered, (op + "@:@").c_str());
    class_addMethod(sub, @selector(draggingUpdated:), (IMP)Hook_draggingUpdated, (op + "@:@").c_str());
    class_addMethod(sub, @selector(draggingExited:), (IMP)Hook_draggingExited, "v@:@");
    class_addMethod(sub, @selector(performDragOperation:), (IMP)Hook_performDragOperation, (boolean + "@:@").c_str());
    class_addMethod(sub, @selector(wantsPeriodicDraggingUpdates), (IMP)Hook_wantsPeriodicDraggingUpdates, (boolean + "@:").c_str());
    objc_registerClassPair(sub);
    return sub;
}

class CocoaDropTarget : public DropTarget
{
public:
    CocoaDropTarget(NSView* view, Class originalClass, NSArray* originalTypes)
        : m_view(view), m_originalClass(originalClass), m_originalTypes([originalTypes retain])
    {
    }

    ~CocoaDropTarget() override
    {
        // The view may already be gone when REAPER recreated the window
        if (IsWindow((HWND)m_view))
        {
            [m_view unregisterDraggedTypes];
            if ([m_originalTypes count])
                [m_view registerForDraggedTypes:m_originalTypes];
            object_setClass(m_view, m_originalClass);
            objc_setAssociatedObject(m_view, &kHandlerKey, nil, OBJC_ASSOCIATION_RETAIN);
        }
        [m_originalTypes release];
    }

private:
    NSView* m_view;
    Class m_originalClass;
    NSArray* m_originalTypes;
};

std::unique_ptr<DropTarget> DropTarget::Attach(HWND hwnd, DragHandler* handler)
{
    id obj = (id)hwnd;
    // A floating mixer is a top-level window: hook its content view
    if ([obj isKindOfClass:[NSWindow class]])
        obj = [(NSWindow*)obj contentView];
    NSView* view = (NSView*)obj;
    if (!view || !handler || ![view isKindOfClass:[NSView class]] || HandlerFor(view))
        return nullptr;

    Class original = object_getClass(view);
    Class hooked = HookedSubclassOf(original);
    if (!hooked) return nullptr;

    NSArray* originalTypes = [view registeredDraggedTypes];
    objc_setAssociatedObject(view, &kHandlerKey, [NSValue valueWithPointer:handler], OBJC_ASSOCIATION_RETAIN);
    object_setClass(view, hooked);
    [view registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSFilenamesPboardType]];

    return std::unique_ptr<DropTarget>(new CocoaDropTarget(view, original, originalTypes));
}

std::unique_ptr<DropTarget> DropTarget::AttachPassthrough(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, bool allowParentWalk)
{
    // TODO: native passthrough for arrange. Not implemented yet.
    (void)hwnd; (void)handler; (void)shouldIntercept; (void)oleFallback; (void)allowParentWalk;
    return nullptr;
}

std::unique_ptr<DropTarget> DropTarget::AttachClaim(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, HWND oleFallback)
{
    (void)hwnd; (void)handler; (void)shouldIntercept; (void)oleFallback;
    return nullptr;
}

namespace platform {
void ReleaseSavedReaperNative() {}
}
