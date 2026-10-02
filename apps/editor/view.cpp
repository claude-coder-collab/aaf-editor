#include "view.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

#define WEBVIEW_HEADER
#include <webview/webview.h>

#ifdef __linux__
    #include <gtk/gtk.h>
#endif

namespace aafedit
{

namespace
{

auto handleOf(void* h) -> webview_t
{
    return static_cast<webview_t>(h);
}

void runTask(webview_t /*view*/, void* arg)
{
    const std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(arg));
    (*task)();
}

void callBinding(const char* id, const char* request, void* arg)
{
    (*static_cast<View::Binding*>(arg))(id, request);
}

}

View::View(bool debug) :
    handle_(webview_create(debug ? 1 : 0, nullptr))
{
    if (handle_ == nullptr)
    {
        throw std::runtime_error("cannot create the webview window");
    }
#ifdef __linux__
    gtk_window_set_icon_name(static_cast<GtkWindow*>(webview_get_window(handleOf(handle_))), "aafedit");
#endif
}

View::~View()
{
    webview_destroy(handleOf(handle_));
}

void View::setTitle(const std::string& title)
{
    webview_set_title(handleOf(handle_), title.c_str());
}

void View::setSize(int width, int height)
{
    webview_set_size(handleOf(handle_), width, height, WEBVIEW_HINT_NONE);
}

void View::setHtml(const std::string& html)
{
    webview_set_html(handleOf(handle_), html.c_str());
}

void View::init(const std::string& js)
{
    webview_init(handleOf(handle_), js.c_str());
}

void View::eval(const std::string& js)
{
    webview_eval(handleOf(handle_), js.c_str());
}

void View::bind(const std::string& name, Binding binding)
{
    bindings_.push_back(std::make_unique<Binding>(std::move(binding)));
    webview_bind(handleOf(handle_), name.c_str(), &callBinding, bindings_.back().get());
}

void View::resolve(const std::string& id, const std::string& json)
{
    webview_return(handleOf(handle_), id.c_str(), 0, json.c_str());
}

void View::dispatch(std::function<void()> task)
{
    webview_dispatch(handleOf(handle_), &runTask, new std::function<void()>(std::move(task)));
}

void View::run()
{
    webview_run(handleOf(handle_));
}

void View::terminate()
{
    webview_terminate(handleOf(handle_));
}

}
