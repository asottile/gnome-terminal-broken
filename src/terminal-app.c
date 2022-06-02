/*
 * Copyright © 2001, 2002 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2002 Sun Microsystems
 * Copyright © 2003 Mariano Suarez-Alvarez
 * Copyright © 2008, 2010, 2011, 2015, 2017 Christian Persch
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

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "terminal-intl.h"
#include "terminal-debug.h"
#include "terminal-app.h"
#include "terminal-accels.h"
#include "terminal-screen.h"
#include "terminal-screen-container.h"
#include "terminal-window.h"
#include "terminal-profiles-list.h"
#include "terminal-util.h"
#include "profile-editor.h"
#include "terminal-schemas.h"
#include "terminal-gdbus.h"
#include "terminal-defines.h"
#include "terminal-prefs.h"
#include "terminal-libgsystem.h"

#ifdef ENABLE_SEARCH_PROVIDER
#include "terminal-search-provider.h"
#endif /* ENABLE_SEARCH_PROVIDER */

#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define DESKTOP_INTERFACE_SETTINGS_SCHEMA       "org.gnome.desktop.interface"

#define SYSTEM_PROXY_SETTINGS_SCHEMA            "org.gnome.system.proxy"

#define GTK_SETTING_PREFER_DARK_THEME           "gtk-application-prefer-dark-theme"

#define GTK_DEBUG_SETTING_SCHEMA                "org.gtk.Settings.Debug"
#define GTK_DEBUG_ENABLE_INSPECTOR_KEY          "enable-inspector-keybinding"
#define GTK_DEBUG_ENABLE_INSPECTOR_TYPE         G_VARIANT_TYPE_BOOLEAN

#ifdef DISUNIFY_NEW_TERMINAL_SECTION
#error Use a gsettings override instead
#endif

/*
 * Session state is stored entirely in the RestartCommand command line.
 *
 * The number one rule: all stored information is EITHER per-session,
 * per-profile, or set from a command line option. THERE CAN BE NO
 * OVERLAP. The UI and implementation totally break if you overlap
 * these categories. See gnome-terminal 1.x for why.
 */

struct _TerminalAppClass {
  GtkApplicationClass parent_class;

  void (* clipboard_targets_changed) (TerminalApp *app,
                                      GtkClipboard *clipboard);
};

struct _TerminalApp
{
  GtkApplication parent_instance;

  GDBusObjectManagerServer *object_manager;

  TerminalSettingsList *profiles_list;

  GHashTable *screen_map;

  GSettings *global_settings;
  GSettings *desktop_interface_settings;
  GSettings *system_proxy_settings;
  GSettings *gtk_debug_settings;

#ifdef ENABLE_SEARCH_PROVIDER
  TerminalSearchProvider *search_provider;
#endif /* ENABLE_SEARCH_PROVIDER */

  GMenuModel *menubar;
  GMenu *menubar_new_terminal_section;
  GMenu *menubar_set_profile_section;

  GMenuModel *profilemenu;
  GMenuModel *headermenu;
  GMenu *headermenu_set_profile_section;

  GMenu *set_profile_menu;

  GtkClipboard *clipboard;
  GdkAtom *clipboard_targets;
  int n_clipboard_targets;

  gboolean unified_menu;
  gboolean use_headerbar;
};

enum
{
  CLIPBOARD_TARGETS_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

/* Debugging helper */

static void
terminal_app_init_debug (void)
{
#ifdef ENABLE_DEBUG
  const char *env = g_getenv ("GTK_TEXT_DIR");
  if (env != NULL) {
    if (g_str_equal (env, "help")) {
      g_printerr ("Usage: GTK_TEXT_DIR=ltr|rtl\n");
    } else {
      GtkTextDirection dir;
      if (g_str_equal (env, "rtl"))
        dir = GTK_TEXT_DIR_RTL;
      else
        dir = GTK_TEXT_DIR_LTR;

      gtk_widget_set_default_direction (dir);
    }
  }

  env = g_getenv ("GTK_SETTINGS");
  if (env == NULL)
    return;

  GObject *settings = G_OBJECT (gtk_settings_get_default ());
  GObjectClass *settings_class = G_OBJECT_GET_CLASS (settings);

  if (g_str_equal (env, "help")) {
    g_printerr ("Usage: GTK_SETTINGS=setting[,setting…] where 'setting' is one of these:\n");

    guint n_props;
    GParamSpec **props = g_object_class_list_properties (settings_class, &n_props);
    for (guint i = 0; i < n_props; i++) {
      if (G_PARAM_SPEC_VALUE_TYPE (props[i]) != G_TYPE_BOOLEAN)
        continue;

      GValue value = { 0, };
      g_value_init (&value, G_TYPE_BOOLEAN);
      g_object_get_property (settings, props[i]->name, &value);
      g_printerr ("  %s (%s)\n", props[i]->name, g_value_get_boolean (&value) ? "true" : "false");
      g_value_unset (&value);
    }
    g_printerr ("  Use 'setting' to set to true, "
                "'~setting' to set to false, "
                "and '!setting' to invert.\n");
  } else {
    gs_strfreev char **tokens = g_strsplit (env, ",", -1);
    for (guint i = 0; tokens[i] != NULL; i++) {
      const char *prop = tokens[i];
      char c = prop[0];
      if (c == '~' || c == '!')
        prop++;

      GParamSpec *pspec = g_object_class_find_property (settings_class, prop);
      if (pspec == NULL) {
        g_printerr ("Setting \"%s\" does not exist.\n", prop);
      } else if (G_PARAM_SPEC_VALUE_TYPE (pspec) != G_TYPE_BOOLEAN) {
        g_printerr ("Setting \"%s\" is not boolean.\n", prop);
      } else {
        GValue value = { 0, };
        g_value_init (&value, G_TYPE_BOOLEAN);
        if (c == '!') {
          g_object_get_property (settings, pspec->name, &value);
          g_value_set_boolean (&value, !g_value_get_boolean (&value));
        } else if (c == '~') {
          g_value_set_boolean (&value, FALSE);
        } else {
          g_value_set_boolean (&value, TRUE);
        }
        g_object_set_property (settings, pspec->name, &value);
        g_value_unset (&value);
      }
    }
  }
#endif
}

/* Helper functions */

static gboolean
strv_contains_gnome (char **strv)
{
  if (strv == NULL)
    return FALSE;

  for (int i = 0; strv[i] != NULL; i++) {
    if (g_ascii_strcasecmp (strv[i], "gnome") == 0 ||
        g_ascii_strcasecmp (strv[i], "gnome-classic") == 0)
      return TRUE;
  }

  return FALSE;
}

/*
 * terminal_app_should_use_headerbar:
 *
 * Determines if the app should use headerbars. This is determined
 * * If the pref is set, the pref value is used
 * * Otherwise, if XDG_CURRENT_DESKTOP contains GNOME or GNOME-Classic,
 *   headerbar is used
 * * Otherwise, headerbar is not used.
 */
static gboolean
terminal_app_should_use_headerbar (TerminalApp *app)
{
  gboolean set, use;
  g_settings_get (app->global_settings, TERMINAL_SETTING_HEADERBAR_KEY, "mb", &set, &use);
  if (set)
    return use;

  const char *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  if (desktop == NULL)
    return FALSE;

  char **desktops = g_strsplit (desktop, G_SEARCHPATH_SEPARATOR_S, -1);
  use = strv_contains_gnome (desktops);
  g_strfreev (desktops);

  return use;
}

static gboolean
load_css_from_resource (GApplication *application,
                        GtkCssProvider *provider,
                        gboolean theme)
{
  const char *base_path;
  gs_free char *uri;
  gs_unref_object GFile *file;
  gs_free_error GError *error = NULL;

  base_path = g_application_get_resource_base_path (application);

  if (theme) {
    gs_free char *str, *theme_name;

    g_object_get (gtk_settings_get_default (), "gtk-theme-name", &str, NULL);
    theme_name = g_ascii_strdown (str, -1);
    uri = g_strdup_printf ("resource://%s/css/%s/terminal.css", base_path, theme_name);
  } else {
    uri = g_strdup_printf ("resource://%s/css/terminal.css", base_path);
  }

  file = g_file_new_for_uri (uri);
  if (!g_file_query_exists (file, NULL /* cancellable */))
    return FALSE;

  if (!gtk_css_provider_load_from_file (provider, file, &error))
    g_assert_no_error (error);

  return TRUE;
}

static void
add_css_provider (GApplication *application,
                  gboolean theme)
{
  gs_unref_object GtkCssProvider *provider;

  provider = gtk_css_provider_new ();
  if (!load_css_from_resource (application, provider, theme))
    return;

  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
app_load_css (GApplication *application)
{
  add_css_provider (application, FALSE);
  add_css_provider (application, TRUE);
}

char *
terminal_app_new_profile (TerminalApp *app,
                          GSettings   *base_profile,
                          const char  *name)
{
  char *uuid;

  if (base_profile) {
    gs_free char *base_uuid;

    base_uuid = terminal_settings_list_dup_uuid_from_child (app->profiles_list, base_profile);
    uuid = terminal_settings_list_clone_child (app->profiles_list, base_uuid, name);
  } else {
    uuid = terminal_settings_list_add_child (app->profiles_list, name);
  }

  return uuid;
}

void
terminal_app_remove_profile (TerminalApp *app,
                             GSettings *profile)
{
  g_return_if_fail (TERMINAL_IS_APP (app));
  g_return_if_fail (G_IS_SETTINGS (profile));

  gs_unref_object GSettings *default_profile = terminal_settings_list_ref_default_child (app->profiles_list);
  if (default_profile == profile)
    return;

  /* First, we need to switch any screen using this profile to the default profile */
  gs_free_list GList *screens = g_hash_table_get_values (app->screen_map);
  for (GList *l = screens; l != NULL; l = l->next) {
    TerminalScreen *screen = TERMINAL_SCREEN (l->data);
    if (terminal_screen_get_profile (screen) != profile)
      continue;

    terminal_screen_set_profile (screen, default_profile);
  }

  /* Now we can safely remove the profile */
  gs_free char *uuid = terminal_settings_list_dup_uuid_from_child (app->profiles_list, profile);
  terminal_settings_list_remove_child (app->profiles_list, uuid);
}

static void
terminal_app_theme_variant_changed_cb (GSettings   *settings,
                                       const char  *key,
                                       GtkSettings *gtk_settings)
{
  TerminalThemeVariant theme;

  theme = g_settings_get_enum (settings, key);
  if (theme == TERMINAL_THEME_VARIANT_SYSTEM)
    gtk_settings_reset_property (gtk_settings, GTK_SETTING_PREFER_DARK_THEME);
  else
    g_object_set (gtk_settings,
                  GTK_SETTING_PREFER_DARK_THEME,
                  theme == TERMINAL_THEME_VARIANT_DARK,
                  NULL);
}

/* Submenus for New Terminal per profile, and to change profiles */

static void terminal_app_update_profile_menus (TerminalApp *app);

typedef struct {
  char *uuid;
  char *label;
} ProfileData;

static void
profile_data_clear (ProfileData *data)
{
  g_free (data->uuid);
  g_free (data->label);
}

typedef struct {
  GArray *array;
  TerminalApp *app;
} ProfilesForeachData;

static void
foreach_profile_cb (TerminalSettingsList *list,
                    const char *uuid,
                    GSettings *profile,
                    ProfilesForeachData *user_data)
{
  ProfileData data;
  data.uuid = g_strdup (uuid);
  data.label = g_settings_get_string (profile, TERMINAL_PROFILE_VISIBLE_NAME_KEY);

  g_array_append_val (user_data->array, data);

  /* only connect if we haven't seen this profile before */
  if (g_signal_handler_find (profile, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
                             0, 0, NULL, terminal_app_update_profile_menus, user_data->app) == 0)
    g_signal_connect_swapped (profile, "changed::" TERMINAL_PROFILE_VISIBLE_NAME_KEY,
                              G_CALLBACK (terminal_app_update_profile_menus), user_data->app);
}

static int
compare_profile_label_cb (gconstpointer ap,
                          gconstpointer bp)
{
  const ProfileData *a = ap;
  const ProfileData *b = bp;

  return g_utf8_collate (a->label, b->label);
}

static void
menu_append_numbered (GMenu *menu,
                      const char *label,
                      int num,
                      const char *action_name,
                      GVariant *target /* floating, consumed */)
{
  gs_free_gstring GString *str;
  gs_unref_object GMenuItem *item;
  const char *p;

  /* Who'd use more that 4 underscores in a profile name... */
  str = g_string_sized_new (strlen (label) + 4 + 1 + 8);

  if (num < 10)
    g_string_append_printf (str, "_%Id. ", num);
  else if (num < 36)
    g_string_append_printf (str, "_%c. ",  (char)('A' + num - 10));

  /* Append the label with underscores elided */
  for (p = label; *p; p++) {
    if (*p == '_')
      g_string_append (str, "__");
    else
      g_string_append_c (str, *p);
  }

  item = g_menu_item_new (str->str, NULL);
  g_menu_item_set_action_and_target_value (item, action_name, target);
  g_menu_append_item (menu, item);
}

static void
append_new_terminal_item (GMenu *section,
                          const char *label,
                          const char *target,
                          ProfileData *data,
                          guint n_profiles)
{
  gs_unref_object GMenuItem *item = g_menu_item_new (label, NULL);

  if (n_profiles > 1) {
    gs_unref_object GMenu *submenu = g_menu_new ();

    for (guint i = 0; i < n_profiles; i++) {
      menu_append_numbered (submenu, data[i].label, i + 1,
                            "win.new-terminal",
                            g_variant_new ("(ss)", target, data[i].uuid));
    }

    g_menu_item_set_link (item, G_MENU_LINK_SUBMENU, G_MENU_MODEL (submenu));
  } else {
    g_menu_item_set_action_and_target (item, "win.new-terminal",
                                       "(ss)", target, "current");
  }
  g_menu_append_item (section, item);
}

static void
fill_header_new_terminal_menu (GMenuModel *menu,
                               ProfileData *data,
                               guint n_profiles)
{
  gs_unref_object GMenu *section = NULL;

  if (n_profiles <= 1)
    return;

  section = g_menu_new ();

  for (guint i = 0; i < n_profiles; i++) {
    menu_append_numbered (section, data[i].label, i + 1,
                          "win.new-terminal",
                          g_variant_new ("(ss)", "default", data[i].uuid));
  }

  g_menu_append_section (G_MENU (menu), _("New Terminal"), G_MENU_MODEL (section));
}

static void
fill_new_terminal_section (TerminalApp *app,
                           GMenu *section,
                           ProfileData *profiles,
                           guint n_profiles)
{
  if (terminal_app_get_menu_unified (app)) {
    append_new_terminal_item (section, _("New _Terminal"), "default", profiles, n_profiles);
  } else {
    append_new_terminal_item (section, _("New _Tab"), "tab", profiles, n_profiles);
    append_new_terminal_item (section, _("New _Window"), "window", profiles, n_profiles);
  }
}

static GMenu *
set_profile_submenu_new (ProfileData *data,
                         guint n_profiles)
{
  /* No submenu if there's only one profile */
  if (n_profiles <= 1)
    return NULL;

  GMenu *menu = g_menu_new ();
  for (guint i = 0; i < n_profiles; i++) {
    menu_append_numbered (menu, data[i].label, i + 1,
                          "win.profile",
                          g_variant_new_string (data[i].uuid));
  }

  return menu;
}

static void
terminal_app_update_profile_menus (TerminalApp *app)
{
  g_clear_object (&app->set_profile_menu);

  /* Get profiles list and sort by label */
  gs_unref_array GArray *array = g_array_sized_new (FALSE, TRUE, sizeof (ProfileData),
                                                    terminal_settings_list_get_n_children (app->profiles_list));
  g_array_set_clear_func (array, (GDestroyNotify) profile_data_clear);

  ProfilesForeachData data = { array, app };
  terminal_settings_list_foreach_child (app->profiles_list,
                                        (TerminalSettingsListForeachFunc) foreach_profile_cb,
                                        &data);
  g_array_sort (array, compare_profile_label_cb);

  ProfileData *profiles = (ProfileData*) array->data;
  guint n_profiles = array->len;

  app->set_profile_menu = set_profile_submenu_new (profiles, n_profiles);

  if (app->menubar != NULL) {
    g_menu_remove_all (G_MENU (app->menubar_new_terminal_section));
    fill_new_terminal_section (app, app->menubar_new_terminal_section, profiles, n_profiles);

    g_menu_remove_all (G_MENU (app->menubar_set_profile_section));
    if (app->set_profile_menu != NULL) {
      g_menu_append_submenu (app->menubar_set_profile_section, _("Change _Profile"),
                             G_MENU_MODEL (app->set_profile_menu));
    }
  }

  if (app->profilemenu != NULL) {
    g_menu_remove_all (G_MENU (app->profilemenu));
    fill_header_new_terminal_menu (app->profilemenu, profiles, n_profiles);
  }

  if (app->headermenu != NULL) {
    g_menu_remove_all (G_MENU (app->headermenu_set_profile_section));
    if (app->set_profile_menu != NULL) {
      g_menu_append_submenu (app->headermenu_set_profile_section, _("_Profile"),
                             G_MENU_MODEL (app->set_profile_menu));
    }
  }
}

static GMenuModel *
terminal_app_create_menubar (TerminalApp *app,
                             gboolean shell_shows_menubar)
{
  /* If the menubar is shown by the shell, omit mnemonics for the submenus. This is because Alt+F etc.
   * are more important to be usable in the terminal, the menu cannot be replaced runtime (to toggle
   * between mnemonic and non-mnemonic versions), gtk-enable-mnemonics or gtk_window_set_mnemonic_modifier()
   * don't effect the menubar either, so there wouldn't be a way to disable Alt+F for File etc. otherwise.
   * Furthermore, the menu would even grab mnemonics from the File and Preferences windows.
   * In Unity, Alt+F10 opens the menubar, this should be good enough for keyboard navigation.
   * If the menubar is shown by the app, toggling mnemonics is handled in terminal-window.c using
   * gtk_window_set_mnemonic_modifier().
   * See bug 792978 for details. */
  terminal_util_load_objects_resource (shell_shows_menubar ? "/org/gnome/terminal/ui/menubar-without-mnemonics.ui"
                                                           : "/org/gnome/terminal/ui/menubar-with-mnemonics.ui",
                                       "menubar", &app->menubar,
                                       "new-terminal-section", &app->menubar_new_terminal_section,
                                       "set-profile-section", &app->menubar_set_profile_section,
                                       NULL);

  /* Install profile sections */
  terminal_app_update_profile_menus (app);

  return app->menubar;
}

static void
terminal_app_create_headermenu (TerminalApp *app)
{
  terminal_util_load_objects_resource ("/org/gnome/terminal/ui/headerbar-menu.ui",
                                       "headermenu", &app->headermenu,
                                       "set-profile-section", &app->headermenu_set_profile_section,
                                       NULL);

  /* Install profile sections */
  terminal_app_update_profile_menus (app);
}

static void
terminal_app_create_profilemenu (TerminalApp *app)
{
  app->profilemenu = G_MENU_MODEL (g_menu_new ());

  /* Install profile sections */
  terminal_app_update_profile_menus (app);
}

/* Clipboard */

static void
free_clipboard_targets (TerminalApp *app)
{
  g_free (app->clipboard_targets);
  app->clipboard_targets = NULL;
  app->n_clipboard_targets = 0;
}

static void
update_clipboard_targets (TerminalApp *app,
                          GdkAtom *targets,
                          int n_targets)
{
  free_clipboard_targets (app);

  /* Sometimes we receive targets == NULL but n_targets == -1 */
  if (targets != NULL) {
    app->clipboard_targets = g_memdup (targets, sizeof (targets[0]) * n_targets);
    app->n_clipboard_targets = n_targets;
  }
}

static void
clipboard_targets_received_cb (GtkClipboard *clipboard,
                               GdkAtom *targets,
                               int n_targets,
                               TerminalApp *app)
{
  update_clipboard_targets (app, targets, n_targets);

  _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_CLIPBOARD) {
    g_printerr ("Clipboard has %d targets:", app->n_clipboard_targets);

    int i;
    for (i = 0; i < app->n_clipboard_targets; i++) {
      gs_free char *atom_name = gdk_atom_name (app->clipboard_targets[i]);
      g_printerr (" %s", atom_name);
    }
    g_printerr ("\n");
  }

  g_signal_emit (app, signals[CLIPBOARD_TARGETS_CHANGED], 0, clipboard);
}

static void
clipboard_owner_change_cb (GtkClipboard *clipboard,
                           GdkEvent *event G_GNUC_UNUSED,
                           TerminalApp *app)
{
  _terminal_debug_print (TERMINAL_DEBUG_CLIPBOARD,
                         "Clipboard owner changed\n");

  clipboard_targets_received_cb (clipboard, NULL, 0, app); /* clear */

  /* We can do this without holding a reference to @app since
   * the app lives as long as the process.
   */
  gtk_clipboard_request_targets (clipboard,
                                 (GtkClipboardTargetsReceivedFunc) clipboard_targets_received_cb,
                                 app);
}

/* Callbacks from former app menu.
 * The preferences one is still used with the "--preferences" cmdline option. */

static void
app_menu_preferences_cb (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  TerminalApp *app = user_data;

  terminal_app_edit_preferences (app, NULL, NULL);
}

static void
app_menu_help_cb (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  terminal_util_show_help (NULL);
}

static void
app_menu_about_cb (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  terminal_util_show_about ();
}

static void
app_menu_quit_cb (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  GtkApplication *application = user_data;
  GtkWindow *window;

  window = gtk_application_get_active_window (application);
  if (TERMINAL_IS_WINDOW (window))
    terminal_window_request_close (TERMINAL_WINDOW (window));
  else /* a dialogue */
    gtk_widget_destroy (GTK_WIDGET (window));
}

/* Class implementation */

G_DEFINE_TYPE (TerminalApp, terminal_app, GTK_TYPE_APPLICATION)

/* GApplicationClass impl */

static void
terminal_app_activate (GApplication *application)
{
  /* No-op required because GApplication is stupid */
}

static void
terminal_app_startup (GApplication *application)
{
  TerminalApp *app = TERMINAL_APP (application);
  const GActionEntry action_entries[] = {
    { "preferences", app_menu_preferences_cb,   NULL, NULL, NULL },
    { "help",        app_menu_help_cb,          NULL, NULL, NULL },
    { "about",       app_menu_about_cb,         NULL, NULL, NULL },
    { "quit",        app_menu_quit_cb,          NULL, NULL, NULL }
  };

  g_application_set_resource_base_path (application, TERMINAL_RESOURCES_PATH_PREFIX);

  G_APPLICATION_CLASS (terminal_app_parent_class)->startup (application);

  /* Need to set the WM class (bug #685742) */
  gdk_set_program_class("Gnome-terminal");

  g_action_map_add_action_entries (G_ACTION_MAP (application),
                                   action_entries, G_N_ELEMENTS (action_entries),
                                   application);

  app_load_css (application);

  /* Figure out whether the shell shows the menubar */
  gboolean shell_shows_menubar;
  g_object_get (gtk_settings_get_default (),
                "gtk-shell-shows-menubar", &shell_shows_menubar,
                NULL);

  /* Create menubar */
  terminal_app_create_menubar (app, shell_shows_menubar);

  /* Keep dynamic menus updated */
  g_signal_connect_swapped (app->profiles_list, "children-changed",
                            G_CALLBACK (terminal_app_update_profile_menus), app);

  /* Show/hide the menubar as appropriate: If the shell wants to show the menubar, make it available. */
  if (shell_shows_menubar)
    gtk_application_set_menubar (GTK_APPLICATION (app),
                                 terminal_app_get_menubar (app));

  _terminal_debug_print (TERMINAL_DEBUG_SERVER, "Startup complete\n");
}

/* GObjectClass impl */

static void
terminal_app_init (TerminalApp *app)
{
  terminal_app_init_debug ();

  gtk_window_set_default_icon_name (GNOME_TERMINAL_ICON_NAME);

  /* Desktop proxy settings */
  app->system_proxy_settings = g_settings_new (SYSTEM_PROXY_SETTINGS_SCHEMA);

  /* Desktop Interface settings */
  app->desktop_interface_settings = g_settings_new (DESKTOP_INTERFACE_SETTINGS_SCHEMA);

  /* Terminal global settings */
  app->global_settings = g_settings_new (TERMINAL_SETTING_SCHEMA);

  /* Gtk debug settings */
  app->gtk_debug_settings = terminal_g_settings_new (GTK_DEBUG_SETTING_SCHEMA,
                                                     GTK_DEBUG_ENABLE_INSPECTOR_KEY,
                                                     GTK_DEBUG_ENABLE_INSPECTOR_TYPE);

  /* These are internal settings that exists only for distributions
   * to override, so we cache them on startup and don't react to changes.
   */
  app->unified_menu = g_settings_get_boolean (app->global_settings, TERMINAL_SETTING_UNIFIED_MENU_KEY);
  app->use_headerbar = terminal_app_should_use_headerbar (app);

  GtkSettings *gtk_settings = gtk_settings_get_default ();
  terminal_app_theme_variant_changed_cb (app->global_settings,
                                         TERMINAL_SETTING_THEME_VARIANT_KEY, gtk_settings);
  g_signal_connect (app->global_settings,
                    "changed::" TERMINAL_SETTING_THEME_VARIANT_KEY,
                    G_CALLBACK (terminal_app_theme_variant_changed_cb),
                    gtk_settings);

  /* Clipboard targets */
  GdkDisplay *display = gdk_display_get_default ();
  app->clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
  clipboard_owner_change_cb (app->clipboard, NULL, app);
  g_signal_connect (app->clipboard, "owner-change",
                    G_CALLBACK (clipboard_owner_change_cb), app);

  if (!gdk_display_supports_selection_notification (display))
    g_printerr ("Display does not support owner-change; copy/paste will be broken!\n");

  /* Get the profiles */
  app->profiles_list = terminal_profiles_list_new ();

  app->screen_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  gs_unref_object GSettings *settings = g_settings_get_child (app->global_settings, "keybindings");
  terminal_accels_init (G_APPLICATION (app), settings, app->use_headerbar);
}

static void
terminal_app_finalize (GObject *object)
{
  TerminalApp *app = TERMINAL_APP (object);

  g_signal_handlers_disconnect_by_func (app->clipboard,
                                        G_CALLBACK (clipboard_owner_change_cb),
                                        app);
  free_clipboard_targets (app);

  g_signal_handlers_disconnect_by_func (app->profiles_list,
                                        G_CALLBACK (terminal_app_update_profile_menus),
                                        app);
  g_hash_table_destroy (app->screen_map);

  g_object_unref (app->global_settings);
  g_object_unref (app->desktop_interface_settings);
  g_object_unref (app->system_proxy_settings);
  g_clear_object (&app->gtk_debug_settings);

  g_clear_object (&app->menubar);
  g_clear_object (&app->menubar_new_terminal_section);
  g_clear_object (&app->menubar_set_profile_section);
  g_clear_object (&app->profilemenu);
  g_clear_object (&app->headermenu);
  g_clear_object (&app->headermenu_set_profile_section);
  g_clear_object (&app->set_profile_menu);

  terminal_accels_shutdown ();

  G_OBJECT_CLASS (terminal_app_parent_class)->finalize (object);
}

static gboolean
terminal_app_dbus_register (GApplication    *application,
                            GDBusConnection *connection,
                            const gchar     *object_path,
                            GError         **error)
{
  TerminalApp *app = TERMINAL_APP (application);
  gs_unref_object TerminalObjectSkeleton *object = NULL;
  gs_unref_object TerminalFactory *factory = NULL;

  if (!G_APPLICATION_CLASS (terminal_app_parent_class)->dbus_register (application,
                                                                       connection,
                                                                       object_path,
                                                                       error))
    return FALSE;

#ifdef ENABLE_SEARCH_PROVIDER
  if (g_settings_get_boolean (app->global_settings, TERMINAL_SETTING_SHELL_INTEGRATION_KEY)) {
    gs_unref_object TerminalSearchProvider *search_provider;

    search_provider = terminal_search_provider_new ();

    if (!terminal_search_provider_dbus_register (search_provider,
                                                 connection,
                                                 TERMINAL_SEARCH_PROVIDER_PATH,
                                                 error))
      return FALSE;

    gs_transfer_out_value (&app->search_provider, &search_provider);
  }
#endif /* ENABLE_SEARCH_PROVIDER */

  object = terminal_object_skeleton_new (TERMINAL_FACTORY_OBJECT_PATH);
  factory = terminal_factory_impl_new ();
  terminal_object_skeleton_set_factory (object, factory);

  app->object_manager = g_dbus_object_manager_server_new (TERMINAL_OBJECT_PATH_PREFIX);
  g_dbus_object_manager_server_export (app->object_manager, G_DBUS_OBJECT_SKELETON (object));

  /* And export the object */
  g_dbus_object_manager_server_set_connection (app->object_manager, connection);
  return TRUE;
}

static void
terminal_app_dbus_unregister (GApplication    *application,
                              GDBusConnection *connection,
                              const gchar     *object_path)
{
  TerminalApp *app = TERMINAL_APP (application);

  if (app->object_manager) {
    g_dbus_object_manager_server_unexport (app->object_manager, TERMINAL_FACTORY_OBJECT_PATH);
    g_object_unref (app->object_manager);
    app->object_manager = NULL;
  }

#ifdef ENABLE_SEARCH_PROVIDER
  if (app->search_provider) {
    terminal_search_provider_dbus_unregister (app->search_provider, connection, TERMINAL_SEARCH_PROVIDER_PATH);
    g_object_unref (app->search_provider);
    app->search_provider = NULL;
  }
#endif /* ENABLE_SEARCH_PROVIDER */

  G_APPLICATION_CLASS (terminal_app_parent_class)->dbus_unregister (application,
                                                                    connection,
                                                                    object_path);
}

static void
terminal_app_class_init (TerminalAppClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);

  object_class->finalize = terminal_app_finalize;

  g_application_class->activate = terminal_app_activate;
  g_application_class->startup = terminal_app_startup;
  g_application_class->dbus_register = terminal_app_dbus_register;
  g_application_class->dbus_unregister = terminal_app_dbus_unregister;

  signals[CLIPBOARD_TARGETS_CHANGED] =
    g_signal_new (I_("clipboard-targets-changed"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalAppClass, clipboard_targets_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, G_TYPE_OBJECT);
}

/* Public API */

GApplication *
terminal_app_new (const char *app_id)
{
  const GApplicationFlags flags = G_APPLICATION_IS_SERVICE;

  return g_object_new (TERMINAL_TYPE_APP,
                       "application-id", app_id ? app_id : TERMINAL_APPLICATION_ID,
                       "flags", flags,
                       NULL);
}

TerminalScreen *
terminal_app_get_screen_by_uuid (TerminalApp *app,
                                 const char  *uuid)
{
  g_return_val_if_fail (TERMINAL_IS_APP (app), NULL);

  return g_hash_table_lookup (app->screen_map, uuid);
}

char *
terminal_app_dup_screen_object_path (TerminalApp *app,
                                     TerminalScreen *screen)
{
  char *object_path = g_strdup_printf (TERMINAL_RECEIVER_OBJECT_PATH_FORMAT,
                                       terminal_screen_get_uuid (screen));
  object_path = g_strdelimit (object_path,  "-", '_');
  g_assert (g_variant_is_object_path (object_path));
  return object_path;
}

/**
 * terminal_app_get_receiver_impl_by_object_path:
 * @app:
 * @object_path:
 *
 * Returns: (transfer full): the #TerminalReceiverImpl for @object_path, or %NULL
 */
static TerminalReceiverImpl *
terminal_app_get_receiver_impl_by_object_path (TerminalApp *app,
                                               const char *object_path)
{
  gs_unref_object GDBusObject *skeleton =
    g_dbus_object_manager_get_object (G_DBUS_OBJECT_MANAGER (app->object_manager),
                                      object_path);
  if (skeleton == NULL || !TERMINAL_IS_OBJECT_SKELETON (skeleton))
    return NULL;

  TerminalReceiverImpl *impl = NULL;
  g_object_get (skeleton, "receiver", &impl, NULL);
  if (impl == NULL)
    return NULL;

  g_assert (TERMINAL_IS_RECEIVER_IMPL (impl));
  return impl;
}

/**
 * terminal_app_get_screen_by_object_path:
 * @app:
 * @object_path:
 *
 * Returns: (transfer full): the #TerminalScreen for @object_path, or %NULL
 */
TerminalScreen *
terminal_app_get_screen_by_object_path (TerminalApp *app,
                                        const char *object_path)
{
  gs_unref_object TerminalReceiverImpl *impl =
    terminal_app_get_receiver_impl_by_object_path (app, object_path);
  if (impl == NULL)
    return NULL;

  return terminal_receiver_impl_get_screen (impl);
}

void
terminal_app_register_screen (TerminalApp *app,
                              TerminalScreen *screen)
{
  const char *uuid = terminal_screen_get_uuid (screen);
  g_hash_table_insert (app->screen_map, g_strdup (uuid), screen);

  gs_free char *object_path = terminal_app_dup_screen_object_path (app, screen);
  TerminalObjectSkeleton *skeleton = terminal_object_skeleton_new (object_path);

  TerminalReceiverImpl *impl = terminal_receiver_impl_new (screen);
  terminal_object_skeleton_set_receiver (skeleton, TERMINAL_RECEIVER (impl));
  g_object_unref (impl);

  g_dbus_object_manager_server_export (app->object_manager,
                                       G_DBUS_OBJECT_SKELETON (skeleton));
}

void
terminal_app_unregister_screen (TerminalApp *app,
                                TerminalScreen *screen)
{
  const char *uuid = terminal_screen_get_uuid (screen);
  gboolean found = g_hash_table_remove (app->screen_map, uuid);
  g_warn_if_fail (found);
  if (!found)
    return; /* repeat unregistering */

  gs_free char *object_path = terminal_app_dup_screen_object_path (app, screen);
  gs_unref_object TerminalReceiverImpl *impl =
    terminal_app_get_receiver_impl_by_object_path (app, object_path);
  g_warn_if_fail (impl != NULL);

  if (impl != NULL)
    terminal_receiver_impl_unset_screen (impl);

  g_dbus_object_manager_server_unexport (app->object_manager, object_path);
}

GdkAtom *
terminal_app_get_clipboard_targets (TerminalApp *app,
                                    GtkClipboard *clipboard,
                                    int *n_targets)
{
  g_return_val_if_fail (TERMINAL_IS_APP (app), NULL);
  g_return_val_if_fail (n_targets != NULL, NULL);

  if (clipboard != app->clipboard) {
    *n_targets = 0;
    return NULL;
  }

  *n_targets = app->n_clipboard_targets;
  return app->clipboard_targets;
}

void
terminal_app_edit_preferences (TerminalApp     *app,
                               GSettings       *profile,
                               const char      *widget_name)
{
  terminal_prefs_show_preferences (profile, widget_name);
}

/**
 * terminal_app_get_profiles_list:
 *
 * Returns: (transfer none): returns the singleton profiles list #TerminalSettingsList
 */
TerminalSettingsList *
terminal_app_get_profiles_list (TerminalApp *app)
{
  return app->profiles_list;
}

/**
 * terminal_app_get_menubar:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the main window menu bar as a #GMenuModel
 */
GMenuModel *
terminal_app_get_menubar (TerminalApp *app)
{
  return app->menubar;
}

/**
 * terminal_app_get_headermenu:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the main window headerbar menu bar as a #GMenuModel
 */
GMenuModel *
terminal_app_get_headermenu (TerminalApp *app)
{
  if (app->headermenu == NULL)
    terminal_app_create_headermenu (app);

  return app->headermenu;
}

/**
 * terminal_app_get_profilemenu:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the main window headerbar profile menu as a #GMenuModel
 */
GMenuModel *
terminal_app_get_profilemenu (TerminalApp *app)
{
  if (app->profilemenu == NULL)
    terminal_app_create_profilemenu (app);

  return app->profilemenu;
}

/**
 * terminal_app_get_profile_section:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the main window's menubar's profiles section as a #GMenuModel
 */
GMenuModel *
terminal_app_get_profile_section (TerminalApp *app)
{
  return G_MENU_MODEL (app->set_profile_menu);
}

/**
 * terminal_app_get_global_settings:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the cached #GSettings object for the org.gnome.Terminal.Preferences schema
 */
GSettings *
terminal_app_get_global_settings (TerminalApp *app)
{
  return app->global_settings;
}

/**
 * terminal_app_get_desktop_interface_settings:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the cached #GSettings object for the org.gnome.interface schema
 */
GSettings *
terminal_app_get_desktop_interface_settings (TerminalApp *app)
{
  return app->desktop_interface_settings;
}

/**
 * terminal_app_get_proxy_settings:
 * @app: a #TerminalApp
 *
 * Returns: (tranfer none): the cached #GSettings object for the org.gnome.system.proxy schema
 */
GSettings *
terminal_app_get_proxy_settings (TerminalApp *app)
{
  return app->system_proxy_settings;
}

GSettings *
terminal_app_get_gtk_debug_settings (TerminalApp *app)
{
  return app->gtk_debug_settings;
}

/**
 * terminal_app_get_system_font:
 * @app:
 *
 * Creates a #PangoFontDescription for the system monospace font.
 *
 * Returns: (transfer full): a new #PangoFontDescription
 */
PangoFontDescription *
terminal_app_get_system_font (TerminalApp *app)
{
  gs_free char *font = NULL;

  g_return_val_if_fail (TERMINAL_IS_APP (app), NULL);

  font = g_settings_get_string (app->desktop_interface_settings, MONOSPACE_FONT_KEY_NAME);

  return pango_font_description_from_string (font);
}

/**
 * FIXME
 */
GDBusObjectManagerServer *
terminal_app_get_object_manager (TerminalApp *app)
{
  g_warn_if_fail (app->object_manager != NULL);
  return app->object_manager;
}

gboolean
terminal_app_get_menu_unified (TerminalApp *app)
{
  g_return_val_if_fail (TERMINAL_IS_APP (app), TRUE);

  return app->unified_menu;
}

gboolean
terminal_app_get_use_headerbar (TerminalApp *app)
{
  g_return_val_if_fail (TERMINAL_IS_APP (app), FALSE);

  return app->use_headerbar;
}

gboolean
terminal_app_get_dialog_use_headerbar (TerminalApp *app)
{
  g_return_val_if_fail (TERMINAL_IS_APP (app), FALSE);

  gboolean dialog_use_header;
  g_object_get (gtk_settings_get_default (),
                "gtk-dialogs-use-header", &dialog_use_header,
                NULL);

  return dialog_use_header && app->use_headerbar;
}
