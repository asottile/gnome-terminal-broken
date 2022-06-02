/*
 * Copyright © 2001, 2002 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2002 Sun Microsystems
 * Copyright © 2003 Mariano Suarez-Alvarez
 * Copyright © 2011, 2013 Christian Persch
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "terminal-client-utils.h"
#include "terminal-defines.h"
#include "terminal-libgsystem.h"

#include <string.h>

#include <gio/gio.h>

#include <gdk/gdk.h>
#if defined(TERMINAL_COMPILATION) && defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#endif

/**
 * terminal_client_append_create_instance_options:
 * @builder: a #GVariantBuilder of #GVariantType "a{sv}"
 * @display: (array element-type=guint8):
 * @startup_id: (array element-type=guint8):
 * @geometry:
 * @role:
 * @profile:
 * @title:
 * @maximise_window:
 * @fullscreen_window:
 *
 * Appends common options to @builder.
 */
void
terminal_client_append_create_instance_options (GVariantBuilder *builder,
                                                const char      *display_name,
                                                const char      *startup_id,
                                                const char      *geometry,
                                                const char      *role,
                                                const char      *profile,
                                                const char      *encoding,
                                                const char      *title,
                                                gboolean         active,
                                                gboolean         maximise_window,
                                                gboolean         fullscreen_window)
{
  /* Bytestring options */
  if (display_name != NULL)
    g_variant_builder_add (builder, "{sv}",
                           "display", g_variant_new_bytestring (display_name));
  if (startup_id)
    g_variant_builder_add (builder, "{sv}",
                           "desktop-startup-id", g_variant_new_bytestring (startup_id));

  /* String options */
  if (profile)
    g_variant_builder_add (builder, "{sv}",
                           "profile", g_variant_new_string (profile));
  if (encoding)
    g_variant_builder_add (builder, "{sv}",
                           "encoding", g_variant_new_string (encoding));
  if (title)
    g_variant_builder_add (builder, "{sv}",
                           "title", g_variant_new_string (title));
  if (geometry)
    g_variant_builder_add (builder, "{sv}",
                           "geometry", g_variant_new_string (geometry));
  if (role)
    g_variant_builder_add (builder, "{sv}",
                           "role", g_variant_new_string (role));

  /* Boolean options */
  if (active)
    g_variant_builder_add (builder, "{sv}",
                           "active", g_variant_new_boolean (active));

  if (maximise_window)
    g_variant_builder_add (builder, "{sv}",
                           "maximize-window", g_variant_new_boolean (TRUE));
  if (fullscreen_window)
    g_variant_builder_add (builder, "{sv}",
                           "fullscreen-window", g_variant_new_boolean (TRUE));
}

/**
 * terminal_client_append_exec_options:
 * @builder: a #GVariantBuilder of #GVariantType "a{sv}"
 * @working_directory: (allow-none): the cwd, or %NULL
 * @shell:
 *
 * Appends the environment and the working directory to @builder.
 */
void
terminal_client_append_exec_options (GVariantBuilder *builder,
                                     const char      *working_directory,
                                     PassFdElement   *fd_array,
                                     gsize            fd_array_len,
                                     gboolean         shell)
{
  gs_strfreev char **envv;

  envv = g_get_environ ();
  envv = g_environ_unsetenv (envv, "COLORTERM");
  envv = g_environ_unsetenv (envv, "COLUMNS");
  envv = g_environ_unsetenv (envv, "DESKTOP_STARTUP_ID");
  envv = g_environ_unsetenv (envv, "GIO_LAUNCHED_DESKTOP_FILE_PID");
  envv = g_environ_unsetenv (envv, "GIO_LAUNCHED_DESKTOP_FILE");
  envv = g_environ_unsetenv (envv, "GNOME_DESKTOP_ICON");
  envv = g_environ_unsetenv (envv, "LINES");
  envv = g_environ_unsetenv (envv, "NOTIFY_SOCKET");
  envv = g_environ_unsetenv (envv, "PWD");
  envv = g_environ_unsetenv (envv, "TERM");
  envv = g_environ_unsetenv (envv, "VTE_VERSION");
  envv = g_environ_unsetenv (envv, "WINDOWID");
  envv = g_environ_unsetenv (envv, TERMINAL_ENV_SERVICE_NAME);
  envv = g_environ_unsetenv (envv, TERMINAL_ENV_SCREEN);

  g_variant_builder_add (builder, "{sv}",
                         "environ",
                         g_variant_new_bytestring_array ((const char * const *) envv, -1));

  if (working_directory)
    g_variant_builder_add (builder, "{sv}",
                           "cwd", g_variant_new_bytestring (working_directory));

  if (shell)
    g_variant_builder_add (builder, "{sv}",
                           "shell",
                           g_variant_new_boolean (TRUE));

  if (fd_array_len) {
    gsize i;

    g_variant_builder_open (builder, G_VARIANT_TYPE ("{sv}"));
    g_variant_builder_add (builder, "s", "fd-set");

    g_variant_builder_open (builder, G_VARIANT_TYPE ("v"));
    g_variant_builder_open (builder, G_VARIANT_TYPE ("a(ih)"));
    for (i = 0; i < fd_array_len; i++) {
      g_variant_builder_add (builder, "(ih)", fd_array[i].fd, fd_array[i].index);
    }
    g_variant_builder_close (builder); /* a(ih) */
    g_variant_builder_close (builder); /* v */

    g_variant_builder_close (builder); /* {sv} */
  }
}

/**
 * terminal_client_get_fallback_startup_id:
 *
 * Returns: a fallback startup ID, or %NULL
 */
char *
terminal_client_get_fallback_startup_id  (void)
{
#if defined(TERMINAL_COMPILATION) && defined(GDK_WINDOWING_X11)
  GdkDisplay *display;
  Display *xdisplay;
  Window xwindow;
  XEvent event;

  display = gdk_display_get_default ();
  if (display == NULL || !GDK_IS_X11_DISPLAY (display))
    goto out;

  xdisplay = GDK_DISPLAY_XDISPLAY (display);

  {
    XSetWindowAttributes attrs;
    Atom atom_name;
    Atom atom_type;
    const char *name;

    attrs.override_redirect = True;
    attrs.event_mask = PropertyChangeMask | StructureNotifyMask;

    xwindow =
      XCreateWindow (xdisplay,
                     RootWindow (xdisplay, 0),
                     -100, -100, 1, 1,
                     0,
                     CopyFromParent,
                     CopyFromParent,
                     (Visual *)CopyFromParent,
                     CWOverrideRedirect | CWEventMask,
                     &attrs);

    atom_name = XInternAtom (xdisplay, "WM_NAME", TRUE);
    g_assert (atom_name != None);
    atom_type = XInternAtom (xdisplay, "STRING", TRUE);
    g_assert (atom_type != None);

    name = "Fake Window";
    XChangeProperty (xdisplay,
                     xwindow, atom_name,
                     atom_type,
                     8, PropModeReplace, (unsigned char *)name, strlen (name));
  }

  XWindowEvent (xdisplay,
                xwindow,
                PropertyChangeMask,
                &event);

  XDestroyWindow(xdisplay, xwindow);

  return g_strdup_printf ("_TIME%lu", event.xproperty.time);
out:
#endif
  return NULL;
}
