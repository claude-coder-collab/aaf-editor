#pragma once

#include <functional>
#include <string>

namespace aafedit
{

/// Receives the UTF-8 local path of a file dropped on the window.
using FileDropHandler = std::function<void(const std::string& path)>;

/// Reports files dropped on the native webview `browser` (a WKWebView*, WebKitWebView* or ICoreWebView2Controller*)
/// to `handler` on the UI thread. A page cannot learn a dropped file's path, so this happens natively:
/// - macOS: the page accepts file drags (it cancels `dragover`), and an override of the web view's
///   `performDragOperation:` reads the file URLs from the drag pasteboard instead of passing the drop to the page;
/// - Linux: the page accepts file drags, and the web view's GTK `drag-drop` / `drag-data-received` signals provide
///   the URIs;
/// - Windows: the page leaves file drops alone, WebView2 navigates to the dropped file, and a `NavigationStarting`
///   handler cancels navigations to `file:` URLs and reports them.
/// `handler` must outlive the webview.
void installDropHandler(void* browser, const FileDropHandler* handler);

}
