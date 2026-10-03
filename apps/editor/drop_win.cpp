#include "drop.hpp"

#include <aaf/rpc/file_url.hpp>

#include <windows.h>

#include <WebView2.h>

#include <atomic>
#include <string>

namespace aafedit
{

namespace
{

auto toUtf8(const wchar_t* text) -> std::string
{
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

class NavigationStarting final : public ICoreWebView2NavigationStartingEventHandler
{
public:
    explicit NavigationStarting(const FileDropHandler* handler) :
        handler_(handler)
    {
    }

    auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) -> HRESULT override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ICoreWebView2NavigationStartingEventHandler)))
        {
            *object = static_cast<ICoreWebView2NavigationStartingEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++references_; }

    auto STDMETHODCALLTYPE Release() -> ULONG override
    {
        const auto left = --references_;
        if (left == 0)
        {
            delete this;
        }
        return left;
    }

    auto STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT override
    {
        LPWSTR uri = nullptr;
        if (FAILED(args->get_Uri(&uri)) || uri == nullptr)
        {
            return S_OK;
        }
        const auto url = toUtf8(uri);
        CoTaskMemFree(uri);
        if (const auto path = aaf::rpc::fileUrlToPath(url))
        {
            args->put_Cancel(TRUE);
            const auto text = path->u8string();
            (*handler_)(std::string(text.begin(), text.end()));
        }
        return S_OK;
    }

private:
    ~NavigationStarting() = default;

    const FileDropHandler* handler_;
    std::atomic<ULONG> references_ = 1;
};

}

void installDropHandler(void* browser, const FileDropHandler* handler)
{
    auto* controller = static_cast<ICoreWebView2Controller*>(browser);
    ICoreWebView2* webview = nullptr;
    if (controller == nullptr || FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr)
    {
        return;
    }
    auto* listener = new NavigationStarting(handler);
    EventRegistrationToken token{};
    webview->add_NavigationStarting(listener, &token);
    listener->Release();
    webview->Release();
}

}
