#include "drop.hpp"

#import <WebKit/WebKit.h>
#include <objc/runtime.h>

@interface AAFFileNavigationDelegate : NSObject <WKNavigationDelegate>
@property (nonatomic, assign) const aafedit::FileNavigationHandler* handler;
@end

@implementation AAFFileNavigationDelegate

- (void)webView:(WKWebView*)webView decidePolicyForNavigationAction:(WKNavigationAction*)action decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
    NSURL* url = action.request.URL;
    if (url.isFileURL && url.path != nil)
    {
        decisionHandler(WKNavigationActionPolicyCancel);
        (*self.handler)(url.path.UTF8String);
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

@end

namespace aafedit
{

namespace
{

char kDelegateKey = 0;

}

void interceptFileNavigation(void* browser, const FileNavigationHandler* handler)
{
    auto* webView = (__bridge WKWebView*)browser;
    AAFFileNavigationDelegate* delegate = [[AAFFileNavigationDelegate alloc] init];
    delegate.handler = handler;
    objc_setAssociatedObject(webView, &kDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    webView.navigationDelegate = delegate;
}

} // namespace aafedit
