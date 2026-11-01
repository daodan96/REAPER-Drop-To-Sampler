// macOS: the TCP/MCP/arrange window is a SWELL NSView. Attaching swaps the view's class
// for a generated subclass that overrides the NSDraggingDestination methods, so file
// drags reach DragHandler instead of REAPER (the same takeover as RegisterDragDrop on
// Windows).
//
// "Native" is REAPER's own handling: SWELL's implementation of the same methods, which
// turns the drop into WM_DROPFILES. It lives on the view itself when the view accepts
// files, otherwise on the nearest superview that does (like the parent OLE target on
// Windows). Attach() never forwards; AttachClaim() / AttachPassthrough() forward
// whenever InterceptFn says the drag is not ours, and switch sides mid-drag.

#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include "platform/platform.h"
#include "drop.h"
#include "settings.h"

#include <cmath>
#include <string>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // NSFilenamesPboardType: what SWELL drags use

static const char* const kSubclassPrefix = "DropToSampler_";
static char kStateKey;

// Per hooked view; owned by CocoaDropTarget
struct HookState
{
    DragHandler* handler = nullptr;
    DropTarget::InterceptFn intercept = nullptr; // null: every media drag is ours
    NSView* native = nil;                        // REAPER's drop view: self, a superview, or nil
    int fileCount = 0;                           // media files in the current drag
    bool ours = false;                           // handler is tracking the drag
    bool nativeActive = false;                   // native has seen draggingEntered
};

static HookState* StateFor(id view)
{
    NSValue* v = objc_getAssociatedObject(view, &kStateKey);
    return v ? (HookState*)[v pointerValue] : nullptr;
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

// The key that picks sampler vs item is not also a drop modifier (as on Windows)
static Modifiers HandlerModifiers(const HookState* st)
{
    Modifiers m = CurrentModifiers();
    if (!st->intercept) return m;
    switch (g_settings.modSurface)
    {
    case ModKey_Shift: m.shift = false; break;
    case ModKey_Ctrl:  m.ctrl = false; break;
    case ModKey_Alt:   m.alt = false; break;
    default: break;
    }
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

// Media files only: anything else stays with REAPER (fileCount 0 => not ours)
static std::vector<std::string> DroppedFiles(id<NSDraggingInfo> info)
{
    std::vector<std::string> files;
    NSPasteboard* pb = [info draggingPasteboard];
    NSArray* urls = [pb readObjectsForClasses:@[[NSURL class]] options:FileUrlOptions()];
    for (NSURL* url in urls)
    {
        const char* path = [[url path] UTF8String];
        if (path && PathIsMediaFile(path)) files.push_back(path);
    }
    if (files.empty())
    {
        NSArray* names = [pb propertyListForType:NSFilenamesPboardType];
        if ([names isKindOfClass:[NSArray class]])
        {
            for (id name in names)
            {
                const char* path = [name isKindOfClass:[NSString class]] ? [(NSString*)name UTF8String] : nullptr;
                if (path && PathIsMediaFile(path)) files.push_back(path);
            }
        }
    }
    return files;
}

// Sends a dragging message to REAPER's handler: the original (SWELL) class when the
// native view is this one, else the superview that accepts files
template <typename R>
static R CallNative(id self, HookState* st, SEL sel, id arg, R fallback)
{
    if (!st || !st->native) return fallback;
    if (st->native != self)
    {
        if (![st->native respondsToSelector:sel]) return fallback;
        return ((R(*)(id, SEL, id))objc_msgSend)(st->native, sel, arg);
    }
    Class base = class_getSuperclass(object_getClass(self));
    if (!class_respondsToSelector(base, sel)) return fallback;
    struct objc_super sup;
    sup.receiver = self;
    sup.super_class = base;
    return ((R(*)(struct objc_super*, SEL, id))objc_msgSendSuper)(&sup, sel, arg);
}

static void CallNativeVoid(id self, HookState* st, SEL sel, id arg)
{
    if (!st || !st->native) return;
    if (st->native != self)
    {
        if ([st->native respondsToSelector:sel])
            ((void (*)(id, SEL, id))objc_msgSend)(st->native, sel, arg);
        return;
    }
    Class base = class_getSuperclass(object_getClass(self));
    if (!class_respondsToSelector(base, sel)) return;
    struct objc_super sup;
    sup.receiver = self;
    sup.super_class = base;
    ((void (*)(struct objc_super*, SEL, id))objc_msgSendSuper)(&sup, sel, arg);
}

static bool WantsOurs(const HookState* st, int x, int y)
{
    if (st->fileCount <= 0) return false;
    if (!st->intercept) return true;
    return st->intercept(CurrentModifiers(), x, y);
}

static void LeaveOurs(HookState* st)
{
    if (!st->ours) return;
    st->handler->OnDragLeave();
    st->ours = false;
}

static void LeaveNative(id self, HookState* st, id<NSDraggingInfo> info)
{
    if (!st->nativeActive) return;
    CallNativeVoid(self, st, @selector(draggingExited:), info);
    st->nativeActive = false;
}

// draggingEntered / draggingUpdated: hand the drag to whichever side wants it now
static NSDragOperation Route(id self, HookState* st, id<NSDraggingInfo> info)
{
    int x, y;
    ScreenPoint(self, info, &x, &y);
    if (WantsOurs(st, x, y))
    {
        LeaveNative(self, st, info);
        if (!st->ours)
        {
            st->handler->OnDragEnter(st->fileCount, x, y, HandlerModifiers(st));
            st->ours = true;
        }
        else
        {
            st->handler->OnDragOver(x, y, HandlerModifiers(st));
        }
        return NSDragOperationCopy;
    }

    LeaveOurs(st);
    if (!st->nativeActive)
    {
        st->nativeActive = true;
        return CallNative<NSDragOperation>(self, st, @selector(draggingEntered:), info, NSDragOperationNone);
    }
    return CallNative<NSDragOperation>(self, st, @selector(draggingUpdated:), info, NSDragOperationNone);
}

static NSDragOperation Hook_draggingEntered(id self, SEL sel, id<NSDraggingInfo> info)
{
    (void)sel;
    HookState* st = StateFor(self);
    if (!st) return NSDragOperationNone;
    st->ours = st->nativeActive = false;
    st->fileCount = (int)DroppedFiles(info).size();
    return Route(self, st, info);
}

static NSDragOperation Hook_draggingUpdated(id self, SEL sel, id<NSDraggingInfo> info)
{
    (void)sel;
    HookState* st = StateFor(self);
    if (!st) return NSDragOperationNone;
    return Route(self, st, info);
}

static void Hook_draggingExited(id self, SEL sel, id<NSDraggingInfo> info)
{
    (void)sel;
    HookState* st = StateFor(self);
    if (!st) return;
    LeaveOurs(st);
    LeaveNative(self, st, info);
    st->fileCount = 0;
}

static BOOL Hook_performDragOperation(id self, SEL sel, id<NSDraggingInfo> info)
{
    HookState* st = StateFor(self);
    if (!st) return NO;

    BOOL ret = NO;
    int x, y;
    ScreenPoint(self, info, &x, &y);
    if (WantsOurs(st, x, y))
    {
        LeaveNative(self, st, info);
        std::vector<std::string> files = DroppedFiles(info);
        st->handler->OnDrop(files, x, y, HandlerModifiers(st));
        st->handler->OnDragLeave();
        ret = YES;
    }
    else
    {
        LeaveOurs(st);
        ret = CallNative<BOOL>(self, st, sel, info, NO);
    }
    st->ours = st->nativeActive = false;
    st->fileCount = 0;
    return ret;
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
    CocoaDropTarget(NSView* view, Class originalClass, NSArray* originalTypes, HookState* state)
        : m_view(view), m_originalClass(originalClass), m_originalTypes([originalTypes retain]), m_state(state)
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
            objc_setAssociatedObject(m_view, &kStateKey, nil, OBJC_ASSOCIATION_RETAIN);
        }
        [m_originalTypes release];
        delete m_state;
    }

private:
    NSView* m_view;
    Class m_originalClass;
    NSArray* m_originalTypes;
    HookState* m_state;
};

static NSView* ViewForHwnd(HWND hwnd)
{
    id obj = (id)hwnd;
    // A floating mixer is a top-level window: hook its content view
    if ([obj isKindOfClass:[NSWindow class]])
        obj = [(NSWindow*)obj contentView];
    return [obj isKindOfClass:[NSView class]] ? (NSView*)obj : nil;
}

static bool AcceptsFileDrops(NSView* view)
{
    NSArray* types = [view registeredDraggedTypes];
    return [types containsObject:NSFilenamesPboardType] || [types containsObject:NSPasteboardTypeFileURL];
}

// The view itself if REAPER accepts file drops on it, else the nearest superview that does
static NSView* NativeDropView(NSView* view)
{
    for (NSView* v = view; v; v = [v superview])
    {
        if (AcceptsFileDrops(v)) return v;
    }
    return nil;
}

static std::unique_ptr<DropTarget> AttachToView(NSView* view, DragHandler* handler,
    DropTarget::InterceptFn intercept, NSView* native)
{
    if (!view || !handler || StateFor(view))
        return nullptr;

    Class original = object_getClass(view);
    Class hooked = HookedSubclassOf(original);
    if (!hooked) return nullptr;

    HookState* st = new HookState;
    st->handler = handler;
    st->intercept = intercept;
    st->native = native;

    NSArray* originalTypes = [view registeredDraggedTypes];
    objc_setAssociatedObject(view, &kStateKey, [NSValue valueWithPointer:st], OBJC_ASSOCIATION_RETAIN);
    object_setClass(view, hooked);
    [view registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSFilenamesPboardType]];

    DebugLog("[Drop To Sampler] hooked %s, native drop view: %s\n", class_getName(original),
        native == view ? "self" : native ? class_getName(object_getClass(native)) : "none");
    return std::unique_ptr<DropTarget>(new CocoaDropTarget(view, original, originalTypes, st));
}

std::unique_ptr<DropTarget> DropTarget::Attach(HWND hwnd, DragHandler* handler)
{
    return AttachToView(ViewForHwnd(hwnd), handler, nullptr, nil);
}

std::unique_ptr<DropTarget> DropTarget::AttachClaim(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, HWND oleFallback)
{
    if (!shouldIntercept) return nullptr;
    NSView* view = ViewForHwnd(hwnd);
    if (!view) return nullptr;
    NSView* native = NativeDropView(view);
    // Floating mixer: no file-accepting view in its own window, use the main window's
    if (!native && oleFallback && oleFallback != hwnd)
        native = NativeDropView(ViewForHwnd(oleFallback));
    return AttachToView(view, handler, shouldIntercept, native);
}

std::unique_ptr<DropTarget> DropTarget::AttachPassthrough(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, bool allowParentWalk)
{
    if (!shouldIntercept) return nullptr;
    NSView* view = ViewForHwnd(hwnd);
    if (!view) return nullptr;
    // Wrap the view REAPER handles drops on (it may be a superview of the arrange view)
    NSView* native = AcceptsFileDrops(view) ? view : allowParentWalk ? NativeDropView(view) : nil;
    if (!native) return nullptr;
    return AttachToView(native, handler, shouldIntercept, native);
}

namespace platform {
void ReleaseSavedReaperNative() {}
}
