#include "drop.hpp"

#include <webkit2/webkit2.h>

namespace aafedit
{

namespace
{

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#if defined(__clang__)
    #pragma GCC diagnostic ignored "-Wcast-function-type-strict"
#endif

auto onDecidePolicy(WebKitWebView* /*view*/, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) -> gboolean
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    {
        return FALSE;
    }
    auto* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const auto* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (uri == nullptr || g_ascii_strncasecmp(uri, "file:", 5) != 0)
    {
        return FALSE;
    }
    gchar* path = g_filename_from_uri(uri, nullptr, nullptr);
    webkit_policy_decision_ignore(decision);
    if (path != nullptr)
    {
        (*static_cast<const FileNavigationHandler*>(data))(path);
        g_free(path);
    }
    return TRUE;
}

}

void interceptFileNavigation(void* browser, const FileNavigationHandler* handler)
{
    g_signal_connect(WEBKIT_WEB_VIEW(browser), "decide-policy", G_CALLBACK(onDecidePolicy), const_cast<FileNavigationHandler*>(handler));
}

#pragma GCC diagnostic pop

}
