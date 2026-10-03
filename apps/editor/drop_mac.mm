#include "drop.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <objc/runtime.h>

namespace
{

char kHandlerKey = 0;
IMP originalPerformDragOperation = nullptr;

auto droppedFiles(id<NSDraggingInfo> info) -> NSArray<NSURL*>*
{
    return [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
}

/// Replaces -[WKWebView performDragOperation:]: a file dropped on a web view with a handler goes to the handler
/// instead of the page; every other drop goes to WebKit as before.
auto performDragOperation(id self, SEL command, id<NSDraggingInfo> info) -> BOOL
{
    NSValue* handler = objc_getAssociatedObject(self, &kHandlerKey);
    NSArray<NSURL*>* files = handler != nullptr ? droppedFiles(info) : nullptr;
    if (files.count == 0 || files.firstObject.path == nullptr)
    {
        return reinterpret_cast<BOOL (*)(id, SEL, id<NSDraggingInfo>)>(originalPerformDragOperation)(self, command, info);
    }
    [static_cast<WKWebView*>(self) draggingExited:info];
    (*static_cast<const aafedit::FileDropHandler*>(handler.pointerValue))(files.firstObject.path.UTF8String);
    return YES;
}

} // namespace

namespace aafedit
{

void installDropHandler(void* browser, const FileDropHandler* handler)
{
    static const bool swizzled = []
    {
        Method method = class_getInstanceMethod(WKWebView.class, @selector(performDragOperation:));
        if (method == nullptr)
        {
            return false;
        }
        originalPerformDragOperation = method_setImplementation(method, reinterpret_cast<IMP>(&performDragOperation));
        return true;
    }();
    if (!swizzled)
    {
        return;
    }
    auto* webView = (__bridge WKWebView*)browser;
    objc_setAssociatedObject(webView, &kHandlerKey, [NSValue valueWithPointer:handler], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

} // namespace aafedit
