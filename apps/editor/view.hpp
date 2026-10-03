#pragma once

#include "drop.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aafedit
{

/// Owns a native webview window. Wraps the webview C API so that the library's implementation is
/// compiled separately with its own settings.
class View
{
public:
    /// Called on the UI thread with the call id and the JSON array of arguments.
    using Binding = std::function<void(const std::string& id, const std::string& request)>;

    explicit View(bool debug);
    ~View();
    View(const View&) = delete;
    View(View&&) = delete;
    auto operator=(const View&) -> View& = delete;
    auto operator=(View&&) -> View& = delete;

    void setTitle(const std::string& title);
    void setSize(int width, int height);
    void setHtml(const std::string& html);
    /// Runs `js` before each page load.
    void init(const std::string& js);
    /// Evaluates `js`; must be called on the UI thread (use `dispatch` from other threads).
    void eval(const std::string& js);
    void bind(const std::string& name, Binding binding);
    /// Opens `url` in the webview.
    void navigate(const std::string& url);
    /// Reports navigations to local files (dropped files) to `handler` instead of opening them (see `drop.hpp`).
    void onFileNavigation(FileNavigationHandler handler);
    /// Completes a bound call; safe from any thread. `json` is the result value as JSON text.
    void resolve(const std::string& id, const std::string& json);
    /// Runs `task` on the UI thread; safe from any thread.
    void dispatch(std::function<void()> task);
    void run();
    void terminate();

private:
    std::unique_ptr<FileNavigationHandler> fileHandler_;
    void* handle_;
    std::vector<std::unique_ptr<Binding>> bindings_;
};

}
