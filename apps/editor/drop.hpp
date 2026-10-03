#pragma once

#include <functional>
#include <string>

namespace aafedit
{

/// Receives the UTF-8 local path of a file the webview was about to open.
using FileNavigationHandler = std::function<void(const std::string& path)>;

/// Hooks the native webview `browser` (a WKWebView*, WebKitWebView* or ICoreWebView2Controller*) so that navigations
/// to `file:` URLs are cancelled and reported to `handler` on the UI thread instead. Every engine navigates to a file
/// that is dropped on a page which does not handle the drop itself, so this is how dropped files arrive.
/// `handler` must outlive the webview.
void interceptFileNavigation(void* browser, const FileNavigationHandler* handler);

}
