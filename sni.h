/* sni.h - KDE StatusNotifier (system tray) integration over sd-bus.
 *
 * Implements org.kde.StatusNotifierItem + com.canonical.dbusmenu so the
 * running process shows an icon in the KDE system tray with a right-click
 * menu ("Exit"). Pure D-Bus, no display/Qt/GLib - only libsystemd (sd-bus).
 */
#ifndef FLICKRFREE_SNI_H
#define FLICKRFREE_SNI_H

#include <stdbool.h>

struct sni;

/* Create the tray item. icon_name is a themed icon name (e.g. "idea").
 * title is shown as the menu label. Returns NULL on fatal failure. */
struct sni *sni_new(const char *icon_name, const char *title,
                    void (*on_exit)(void *userdata), void *userdata);

/* fd to poll for readable events (add to your poll set). Returns <0 if none. */
int sni_get_fd(struct sni *s);

/* Process ready D-Bus events. Call when sni_get_fd() is readable (or once per
 * loop iteration). Returns 0 normally. */
int sni_process(struct sni *s);

/* True once the tray "Exit" menu item was activated (or on_exit fired). */
bool sni_wants_exit(struct sni *s);

void sni_destroy(struct sni *s);

#endif
