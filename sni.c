/* sni.c - KDE StatusNotifier (system tray) integration over sd-bus.
 *
 * Uses sd-bus (libsystemd). Two D-Bus objects are registered:
 *   - /StatusNotifierItem (org.kde.StatusNotifierItem)   the tray icon entry
 *   - /Menu (com.canonical.dbusmenu)                     the right-click menu
 * and the item is registered with org.kde.StatusNotifierWatcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include "sni.h"

#define MENU_PATH "/Menu"
#define SNI_PATH  "/StatusNotifierItem"

struct sni {
    sd_bus *bus;
    char *icon;
    char *title;
    void (*on_exit)(void *userdata);
    void *userdata;
    bool wants_exit;
};

/* One explicit typed getter for every property on both interfaces. Using a
 * NULL getter with an offset is a trap: sd-bus would read garbage from the
 * struct for the string/pointer types. */
static int prop_get(sd_bus *bus, const char *path, const char *iface,
                    const char *prop, sd_bus_message *reply, void *ud,
                    sd_bus_error *error)
{
    struct sni *s = ud;
    (void)bus; (void)path; (void)error;

    if (!strcmp(iface, "org.kde.StatusNotifierItem")) {
        if (!strcmp(prop, "Category"))  return sd_bus_message_append(reply, "s", "ApplicationStatus");
        if (!strcmp(prop, "Id"))        return sd_bus_message_append(reply, "s", "flickrfree");
        if (!strcmp(prop, "Title"))     return sd_bus_message_append(reply, "s", s->title);
        if (!strcmp(prop, "Status"))    return sd_bus_message_append(reply, "s", "Active");
        if (!strcmp(prop, "WindowId"))  return sd_bus_message_append(reply, "i", 0);
        if (!strcmp(prop, "IconName"))  return sd_bus_message_append(reply, "s", s->icon);
        if (!strcmp(prop, "OverlayIconName")) return sd_bus_message_append(reply, "s", "");
        if (!strcmp(prop, "Menu"))      return sd_bus_message_append(reply, "o", MENU_PATH);
        if (!strcmp(prop, "ItemIsMenu"))return sd_bus_message_append(reply, "b", 0);
        if (!strcmp(prop, "IconThemePath")) return sd_bus_message_append(reply, "s", "");
        if (!strcmp(prop, "IconPixmap")) {
            int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
            if (r < 0) return r;
            return sd_bus_message_close_container(reply);
        }
        if (!strcmp(prop, "OverlayIconPixmap") || !strcmp(prop, "AttentionIconPixmap")) {
            int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
            if (r < 0) return r;
            return sd_bus_message_close_container(reply);
        }
        if (!strcmp(prop, "AttentionIconName") || !strcmp(prop, "AttentionMovieName")) {
            return sd_bus_message_append(reply, "s", "");
        }
        if (!strcmp(prop, "ToolTip")) {
            int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
            if (r < 0) return r;
            sd_bus_message_append(reply, "s", s->icon ? s->icon : "");
            sd_bus_message_open_container(reply, 'a', "(iiay)");
            sd_bus_message_close_container(reply);
            sd_bus_message_append(reply, "ss", s->title, "Holds your VRR panel at max refresh.");
            return sd_bus_message_close_container(reply);
        }
        return sd_bus_message_append(reply, "s", "");   /* fallback */
    }

    if (!strcmp(iface, "com.canonical.dbusmenu")) {
        if (!strcmp(prop, "Version"))       return sd_bus_message_append(reply, "i", 3);
        if (!strcmp(prop, "TextDirection")) return sd_bus_message_append(reply, "s", "ltr");
        if (!strcmp(prop, "Status"))        return sd_bus_message_append(reply, "s", "normal");
        if (!strcmp(prop, "IconThemePath")) return sd_bus_message_append(reply, "s", "");
        return sd_bus_message_append(reply, "s", "");
    }

    return sd_bus_message_append(reply, "s", "");
}

/* ---------------- dbusmenu methods ---------------- */

static int dbusmenu_GetLayout(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    struct sni *s = ud;
    int32_t parent_id, recurse;
    parent_id = 0; recurse = -1;
    sd_bus_message_read(m, "ii", &parent_id, &recurse);
    sd_bus_message_skip(m, "as");          /* propertyNames (unused) */
    (void)e;

    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    sd_bus_message_append(reply, "u", (uint32_t)3);      /* revision */

    sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    /* ---- root menu item (id 0) ---- */
    sd_bus_message_open_container(reply, 'r', "ia{sv}");
    sd_bus_message_append(reply, "i", 0);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    sd_bus_message_append(reply, "{sv}", "label", "s", s->title);
    sd_bus_message_append(reply, "{sv}", "enabled", "b", 1);
    sd_bus_message_append(reply, "{sv}", "visible", "b", 1);
    sd_bus_message_append(reply, "{sv}", "children-display", "s", "submenu");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    /* ---- "Exit" item (id 1) ---- */
    sd_bus_message_open_container(reply, 'r', "ia{sv}");
    sd_bus_message_append(reply, "i", 1);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    sd_bus_message_append(reply, "{sv}", "label", "s", "Exit");
    sd_bus_message_append(reply, "{sv}", "enabled", "b", 1);
    sd_bus_message_append(reply, "{sv}", "visible", "b", 1);
    sd_bus_message_append(reply, "{sv}", "type", "s", "standard");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply); /* a(ia{sv}) */

    return sd_bus_send(NULL, reply, NULL);
}

static int dbusmenu_GetProperty(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    (void)ud; (void)e;
    int32_t id;
    const char *name;
    sd_bus_message_read(m, "is", &id, &name);
    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    /* no known extra properties */
    sd_bus_message_append_basic(reply, 'v', "");
    return sd_bus_send(NULL, reply, NULL);
}

static int dbusmenu_GetGroupProperties(sd_bus_message *m, void *ud,
                                       sd_bus_error *e)
{
    (void)ud; (void)e;
    sd_bus_message_skip(m, "aias");
    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    sd_bus_message_append(reply, "a(ia{sv})");
    return sd_bus_send(NULL, reply, NULL);
}

static int dbusmenu_Event(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    struct sni *s = ud;
    int32_t id;
    const char *event_id;
    uint32_t timestamp;
    (void)e;

    sd_bus_message_read(m, "is", &id, &event_id);
    /* skip the variant data */
    sd_bus_message_skip(m, "v");
    sd_bus_message_read(m, "u", &timestamp);
    (void)timestamp;

    if (id == 1 && event_id && strcmp(event_id, "clicked") == 0) {
        s->wants_exit = true;
        if (s->on_exit)
            s->on_exit(s->userdata);
    }
    return sd_bus_reply_method_return(m, NULL);
}

/* ---------------- vlables ---------------- */

static const sd_bus_vtable sni_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("WindowId", "i", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconName", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("OverlayIconName", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AttentionIconName", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AttentionMovieName", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ItemIsMenu", "b", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("OverlayIconPixmap", "a(iiay)", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AttentionIconPixmap", "a(iiay)", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "i", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IconThemePath", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetLayout", "iias", "ua(ia{sv})",
                  dbusmenu_GetLayout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})",
                  dbusmenu_GetGroupProperties, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetProperty", "is", "v",
                  dbusmenu_GetProperty, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", dbusmenu_Event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

struct sni *sni_new(const char *icon_name, const char *title,
                    void (*on_exit)(void *userdata), void *userdata)
{
    int r;
    struct sni *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->icon = icon_name ? strdup(icon_name) : strdup("idea");
    s->title = title ? strdup(title) : strdup("FlickrFree");
    s->on_exit = on_exit;
    s->userdata = userdata;

    r = sd_bus_open_user(&s->bus);
    if (r < 0) {
        fprintf(stderr, "flickrfree: sd_bus_open_user: %s\n", strerror(-r));
        goto fail;
    }

    r = sd_bus_add_object_vtable(s->bus, NULL, SNI_PATH,
                                 "org.kde.StatusNotifierItem", sni_vtable, s);
    if (r < 0) { fprintf(stderr, "flickrfree: sni vtable: %s\n", strerror(-r)); goto fail; }

    r = sd_bus_add_object_vtable(s->bus, NULL, MENU_PATH,
                                 "com.canonical.dbusmenu", menu_vtable, s);
    if (r < 0) { fprintf(stderr, "flickrfree: menu vtable: %s\n", strerror(-r)); goto fail; }

    /* own a unique, stable service name for the item */
    char *name = NULL;
    if (asprintf(&name, "org.kde.StatusNotifierItem-%ld-1", (long)getpid()) < 0)
        name = NULL;
    if (name) {
        sd_bus_call_method(s->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "RequestName",
                           NULL, NULL, "su", name, (uint32_t)0);
    }

    /* register with the KDE tray watcher (non-fatal if tray absent) */
    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(s->bus, "org.kde.StatusNotifierWatcher",
                           "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher",
                           "RegisterStatusNotifierItem", &err, NULL, "s",
                           name ? name : SNI_PATH);
    if (r < 0) {
        fprintf(stderr, "flickrfree: tray register: %s\n", err.message ? err.message : "?");
        sd_bus_error_free(&err);
    }
    free(name);
    return s;

fail:
    if (s->bus) sd_bus_unref(s->bus);
    free(s->icon);
    free(s->title);
    free(s);
    return NULL;
}

int sni_get_fd(struct sni *s)
{
    if (!s || !s->bus) return -1;
    return sd_bus_get_fd(s->bus);
}

int sni_process(struct sni *s)
{
    if (!s || !s->bus) return 0;
    int r;
    while ((r = sd_bus_process(s->bus, NULL)) > 0) {
        /* keep pumping */
    }
    if (r < 0) {
        /* connection dropped; nothing more we can do */
    }
    sd_bus_wait(s->bus, 0);
    return 0;
}

bool sni_wants_exit(struct sni *s)
{
    return s && s->wants_exit;
}

void sni_destroy(struct sni *s)
{
    if (!s) return;
    if (s->bus) {
        sd_bus_unref(s->bus);
    }
    free(s->icon);
    free(s->title);
    free(s);
}
