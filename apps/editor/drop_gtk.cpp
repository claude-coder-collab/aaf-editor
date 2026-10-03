#include "drop.hpp"

#include <webkit2/webkit2.h>

namespace aafedit
{

namespace
{

/// WebKit asks for the drag data both while the pointer moves and after the drop, so only data that arrives after
/// `drag-drop` is a drop. (GTK emits `drag-leave` before `drag-drop`, so leaving does not reset the flag.)
struct DropState
{
    const FileDropHandler* handler = nullptr;
    bool dropping = false;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#if defined(__clang__)
    #pragma GCC diagnostic ignored "-Wcast-function-type-strict"
#endif

auto onDragDrop(GtkWidget* /*widget*/, GdkDragContext* /*context*/, gint /*x*/, gint /*y*/, guint /*time*/, gpointer data) -> gboolean
{
    static_cast<DropState*>(data)->dropping = true;
    return FALSE;
}

void onDragDataReceived(GtkWidget* /*widget*/, GdkDragContext* /*context*/, gint /*x*/, gint /*y*/, GtkSelectionData* selection, guint /*info*/, guint /*time*/, gpointer data)
{
    auto* state = static_cast<DropState*>(data);
    if (!state->dropping)
    {
        return;
    }
    gchar** uris = gtk_selection_data_get_uris(selection);
    if (uris == nullptr)
    {
        return;
    }
    state->dropping = false;
    for (gchar** uri = uris; *uri != nullptr; ++uri)
    {
        if (gchar* path = g_filename_from_uri(*uri, nullptr, nullptr); path != nullptr)
        {
            (*state->handler)(path);
            g_free(path);
            break;
        }
    }
    g_strfreev(uris);
}

void freeState(gpointer data)
{
    delete static_cast<DropState*>(data);
}

}

void installDropHandler(void* browser, const FileDropHandler* handler)
{
    auto* widget = GTK_WIDGET(browser);
    auto* state = new DropState{ handler, false };
    g_object_set_data_full(G_OBJECT(widget), "aafedit-drop", state, freeState);
    g_signal_connect(widget, "drag-drop", G_CALLBACK(onDragDrop), state);
    g_signal_connect(widget, "drag-data-received", G_CALLBACK(onDragDataReceived), state);
}

#pragma GCC diagnostic pop

}
