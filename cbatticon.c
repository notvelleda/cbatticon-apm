/*
 * Copyright (C) 2011-2013 Colin Jones
 * Copyright (C) 2014-2023 Valère Monseur
 *
 * Based on code by Matteo Marchesotti
 * Copyright (C) 2007 Matteo Marchesotti <matteo.marchesotti@fsfe.org>
 *
 * cbatticon: a lightweight and fast battery icon that sits in your system tray.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define CBATTICON_VERSION_NUMBER 1.6.13
#define CBATTICON_VERSION_STRING "1.6.13"
#define CBATTICON_STRING         "cbatticon-apm"

#include <glib.h>
#include <gtk/gtk.h>
#ifdef WITH_NOTIFY
#include <libnotify/notify.h>
#endif

/* for readlink() */
#define __USE_XOPEN_EXTENDED

#include <apm.h>
#include "eggtrayicon.h"
#include <getopt.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#if 0
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#else

/* replaces gi18n.h */
#define  _(String) gettext (String)
#define N_(String) (String)
#define g_dngettext dngettext

/* replaces gprintf.h */
#include <stdio.h>
#define g_printf printf
#define g_sprintf sprintf

#include <string.h>
#define g_strcmp0 strcmp

#endif

#define DEFAULT_UPDATE_INTERVAL 5
#define DEFAULT_LOW_LEVEL       20
#define DEFAULT_CRITICAL_LEVEL  5

#define STR_LTH 256

enum {
    UNKNOWN_ICON = 0,
    BATTERY_ICON_STANDARD,
    BATTERY_ICON_NOTIFICATION,
    BATTERY_ICON_GPM
};

enum {
    MISSING = 0,
    UNKNOWN,
    CHARGED,
    CHARGING,
    DISCHARGING,
    NOTCHARGING,
    LOW_LEVEL,
    CRITICAL_LEVEL
};

struct configuration {
    gboolean display_version;
    gboolean debug_output;
    gint     update_interval;
    gint     icon_type;
    gint     low_level;
    gint     critical_level;
    gchar   *command_low_level;
    gchar   *command_critical_level;
    gchar   *command_left_click;
#ifdef WITH_NOTIFY
    gboolean hide_notification;
#endif
    gboolean list_icon_types;
} configuration = {
    FALSE,
    FALSE,
    DEFAULT_UPDATE_INTERVAL,
    UNKNOWN_ICON,
    DEFAULT_LOW_LEVEL,
    DEFAULT_CRITICAL_LEVEL,
    NULL,
    NULL,
#ifdef WITH_NOTIFY
    FALSE,
#endif
    FALSE,
    FALSE
};

struct icon {
    EggTrayIcon *egg_tray_icon;
    GtkWidget *image;
    GtkTooltips *tooltips;
    gchar *name;
    gint size;
};

GHashTable *icons_cache;

static gint get_options (int argc, char **argv);
static void print_usage ();

static gboolean get_battery_status (apm_info *info, gint *status);

static gboolean get_battery_charge (apm_info *info, gboolean remaining, gint *percentage, gint *time);
static gboolean get_battery_time_estimation (gdouble remaining_capacity, gdouble y, gint *time);
static void reset_battery_time_estimation (void);

static void create_tray_icon (void);
static void set_tray_icon (struct icon *tray_icon, const gchar *name);
static gboolean update_tray_icon (struct icon *tray_icon);
static void update_tray_icon_status (struct icon *tray_icon);
static gboolean on_tray_icon_click (struct icon *tray_icon, GdkEventButton *event, gpointer user_data);

#ifdef WITH_NOTIFY
static void notify_message (NotifyNotification **notification, gchar *summary, gchar *body, gint timeout, NotifyUrgency urgency);
#define NOTIFY_MESSAGE(...) notify_message(__VA_ARGS__)
#else
#define NOTIFY_MESSAGE(...)
#endif

static gchar* get_tooltip_string (gchar *battery, gchar *time);
static gchar* get_battery_string (gint state, gint percentage);
static gchar* get_time_string (gint minutes);
static gchar* get_icon_name (gint state, gint percentage);
static char *get_icon_path (const gchar *name);

/*
 * workaround for limited/bugged batteries/drivers that don't provide current rate
 * the next 4 variables are used to calculate estimated time
 */

static gdouble  estimation_remaining_capacity = -1;
static gint     estimation_time               = -1;
static GTimer  *estimation_timer              = NULL;

#define ICON_PATH_MAX_LEN 256

static char *get_icon_path (const gchar *name)
{
    static char path[ICON_PATH_MAX_LEN];
    ssize_t link_length = readlink("/proc/self/exe", path, ICON_PATH_MAX_LEN - 1);
    g_assert (link_length > 0);
    path[link_length] = 0;

    if (configuration.debug_output == TRUE) {
        g_printf ("executable path is \"%s\"\n", path);
    }

    char *prefix_offset = strstr(path, "/bin/");

    if (prefix_offset == NULL) {
        strcpy(path, "/usr");
        prefix_offset = &path[4];
    }

    g_assert ((prefix_offset - path) + 25 + strlen(name) + 4 + 1 < ICON_PATH_MAX_LEN);
    g_sprintf (prefix_offset, "/share/pixmaps/cbatticon/%s.png", name);

    if (configuration.debug_output == TRUE) {
        g_printf ("icon path is \"%s\"\n", path);
    }

    return path; /* this is Not Very Good but it should be good enough here */
}

/*
 * command line options function
 */

static gint get_options (int argc, char **argv)
{
    /*GError *error = NULL;*/

    gchar *icon_type_string = NULL;

    /*GOptionEntry option_entries[] = {
        { "version"               , 'v', 0, G_OPTION_ARG_NONE  , &configuration.display_version       , N_("Display the version")                                      , NULL },
        { "debug"                 , 'd', 0, G_OPTION_ARG_NONE  , &configuration.debug_output          , N_("Display debug information")                                , NULL },
        { "update-interval"       , 'u', 0, G_OPTION_ARG_INT   , &configuration.update_interval       , N_("Set update interval (in seconds)")                         , NULL },
        { "icon-type"             , 'i', 0, G_OPTION_ARG_STRING, &icon_type_string                    , N_("Set icon type ('standard', 'notification' or 'symbolic')") , NULL },
        { "low-level"             , 'l', 0, G_OPTION_ARG_INT   , &configuration.low_level             , N_("Set low battery level (in percent)")                       , NULL },
        { "critical-level"        , 'r', 0, G_OPTION_ARG_INT   , &configuration.critical_level        , N_("Set critical battery level (in percent)")                  , NULL },
        { "command-low-level"     , 'o', 0, G_OPTION_ARG_STRING, &configuration.command_low_level     , N_("Command to execute when low battery level is reached")     , NULL },
        { "command-critical-level", 'c', 0, G_OPTION_ARG_STRING, &configuration.command_critical_level, N_("Command to execute when critical battery level is reached"), NULL },
        { "command-left-click"    , 'x', 0, G_OPTION_ARG_STRING, &configuration.command_left_click    , N_("Command to execute when left clicking on tray icon")       , NULL },
#ifdef WITH_NOTIFY
        { "hide-notification"     , 'n', 0, G_OPTION_ARG_NONE  , &configuration.hide_notification     , N_("Hide the notification popups")                             , NULL },
#endif
        { "list-icon-types"       , 't', 0, G_OPTION_ARG_NONE  , &configuration.list_icon_types       , N_("List available icon types")                                , NULL },
        { NULL }
    };*/

    static struct option long_options[] = {
        { "help",                   no_argument, NULL, 'h' },
        { "version",                no_argument, NULL, 'v' },
        { "debug",                  no_argument, NULL, 'd' },
        { "update-interval",        no_argument, NULL, 'u' },
        { "icon-type",              no_argument, NULL, 'i' },
        { "low-level",              no_argument, NULL, 'l' },
        { "critical-level",         no_argument, NULL, 'r' },
        { "command-low-level",      no_argument, NULL, 'o' },
        { "command-critical-level", no_argument, NULL, 'c' },
        { "command-left-click",     no_argument, NULL, 'x' },
#ifdef WITH_NOTIFY
        { "hide-notification",      no_argument, NULL, 'n' },
#endif
        { "list-icon-types",        no_argument, NULL, 't' },
        { NULL }
    };

    /*option_context = g_option_context_new (_("[BATTERY ID]"));
    g_option_context_add_main_entries (option_context, option_entries, CBATTICON_STRING);

    if (g_option_context_parse (option_context, &argc, &argv, &error) == FALSE) {
        g_printerr (_("Cannot parse command line arguments: %s\n"), error->message);
        g_error_free (error); error = NULL;

        return -1;
    }

    g_option_context_free (option_context);*/

    while (1) {
        int option_index = 0;

        int c = getopt_long (argc, argv,
                         "hvdu:i:l:r:o:c:x:"
#ifdef WITH_NOTIFY
                         "n"
#endif
                         "t",
                         long_options, &option_index);

        if (c == -1)
            break;

        if (c == 0) {
            c = long_options[option_index].val;
        }

        switch (c) {
            case 'h':
                print_usage ();
                exit (0);
            case 'v':
                configuration.display_version = TRUE;
                break;
            case 'd':
                configuration.debug_output = TRUE;
                break;
#ifdef WITH_NOTIFY
            case 'n':
                configuration.hide_notification = TRUE;
                break;
#endif
            case 't':
                configuration.list_icon_types = TRUE;
                break;
            case 'u':
                configuration.update_interval = strtol (optarg, NULL, 10);
                break;
            case 'i':
                icon_type_string = g_strdup (optarg);
                break;
            case 'l':
                configuration.low_level = strtol (optarg, NULL, 10);
                break;
            case 'r':
                configuration.critical_level = strtol (optarg, NULL, 10);
                break;
            case 'o':
                configuration.command_low_level = g_strdup (optarg);
                break;
            case 'c':
                configuration.command_critical_level = g_strdup (optarg);
                break;
            case 'x':
                configuration.command_left_click = g_strdup (optarg);
                break;
            default:
                abort ();
        }
    }

    /* option : display the version */

    if (configuration.display_version == TRUE) {
        g_print (_("cbatticon: a lightweight and fast battery icon that sits in your system tray\n"));
        g_print (_("version %s\n"), CBATTICON_VERSION_STRING);

        return 0;
    }

    /* option : list available icon types */

    gtk_init (&argc, &argv); /* gtk is required as from this point */

    #define HAS_STANDARD_ICON_TYPE      (access (get_icon_path ("battery-full"), F_OK) == 0)
    #define HAS_NOTIFICATION_ICON_TYPE  (access (get_icon_path ("notification-battery-100"), F_OK) == 0)
    #define HAS_GPM_ICON_TYPE           (access (get_icon_path ("gpm-primary-100"), F_OK) == 0)

    if (configuration.list_icon_types == TRUE) {
        g_print (_("List of available icon types:\n"));
        g_print ("standard\t%s\n"    , HAS_STANDARD_ICON_TYPE     == TRUE ? _("available") : _("unavailable"));
        g_print ("notification\t%s\n", HAS_NOTIFICATION_ICON_TYPE == TRUE ? _("available") : _("unavailable"));
        g_print ("gpm\t\t%s\n"       , HAS_GPM_ICON_TYPE          == TRUE ? _("available") : _("unavailable"));

        return 0;
    }

    /* option : set icon type */

    if (icon_type_string != NULL) {
        if (g_strcmp0 (icon_type_string, "standard") == 0 && HAS_STANDARD_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_STANDARD;
        else if (g_strcmp0 (icon_type_string, "notification") == 0 && HAS_NOTIFICATION_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_NOTIFICATION;
        else if (g_strcmp0 (icon_type_string, "gpm") == 0 && HAS_GPM_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_GPM;
        else g_printerr (_("Unknown icon type: %s\n"), icon_type_string);

        g_free (icon_type_string);
    }

    if (configuration.icon_type == UNKNOWN_ICON) {
        if (HAS_STANDARD_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_STANDARD;
        else if (HAS_NOTIFICATION_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_NOTIFICATION;
        else if (HAS_GPM_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_GPM;
        else g_printerr (_("No icon type found!\n"));
    }

    /* option : update interval */

    if (configuration.update_interval <= 0) {
        configuration.update_interval = DEFAULT_UPDATE_INTERVAL;
        g_printerr (_("Invalid update interval! It has been reset to default (%d seconds)\n"), DEFAULT_UPDATE_INTERVAL);
    }

    /* option : low and critical levels */

    if (configuration.low_level < 0 || configuration.low_level > 100) {
        configuration.low_level = DEFAULT_LOW_LEVEL;
        g_printerr (_("Invalid low level! It has been reset to default (%d percent)\n"), DEFAULT_LOW_LEVEL);
    }

    if (configuration.critical_level < 0 || configuration.critical_level > 100) {
        configuration.critical_level = DEFAULT_CRITICAL_LEVEL;
        g_printerr (_("Invalid critical level! It has been reset to default (%d percent)\n"), DEFAULT_CRITICAL_LEVEL);
    }

    if (configuration.critical_level > configuration.low_level) {
        configuration.critical_level = DEFAULT_CRITICAL_LEVEL;
        configuration.low_level = DEFAULT_LOW_LEVEL;
        g_printerr (_("Critical level is higher than low level! They have been reset to default\n"));
    }

    return 1;
}

static void print_usage ()
{
    g_printf("Usage:\n"
             "  cbatticon [OPTION...]\n"
             "\n"
             "Help Options:\n"
             "  -h, --help                       Show help options\n"
             "\n"
             "Application Options:\n"
             "  -v, --version                    Display the version\n"
             "  -d, --debug                      Display debug information\n"
             "  -u, --update-interval            Set update interval (in seconds)\n"
             "  -i, --icon-type                  Set icon type ('standard', 'notification' or 'gpm')\n"
             "  -l, --low-level                  Set low battery level (in percent)\n"
             "  -r, --critical-level             Set critical battery level (in percent)\n"
             "  -o, --command-low-level          Command to execute when low battery level is reached\n"
             "  -c, --command-critical-level     Command to execute when critical battery level is reached\n"
             "  -x, --command-left-click         Command to execute when left clicking on tray icon\n"
#ifdef WITH_NOTIFY
             "  -n, --hide-notification          Hide the notification popups\n"
#endif
             "  -t, --list-icon-types            List available icon types\n");
}

/*
 * sysfs functions
 */

static gboolean get_battery_status (apm_info *info, gint *status)
{
    g_return_val_if_fail (status != NULL, FALSE);
    g_return_val_if_fail (info != NULL, FALSE);

    switch (info->battery_status) {
#if 0
        case 0x00: /* High */
            *status = DISCHARGING;
            break;
        case 0x01: /* Low */
            *status = LOW_LEVEL;
            break;
        case 0x02: /* Critical */
            *status = CRITICAL_LEVEL;
            break;
#endif
        case 0x00: /* High */
        case 0x01: /* Low */
        case 0x02: /* Critical */
            *status = DISCHARGING;
            break;
        case 0x03: /* Charging */
            if (info->battery_percentage == 100) {
                *status = CHARGED;
            } else {
                *status = CHARGING;
            }
            break;
        case 0x04: /* Selected battery not present */
            *status = MISSING;
            break;
        default:
            *status = UNKNOWN;
            break;
    }

    if (info->battery_flags & (1 << 3)) {
        if (info->battery_percentage == 100) {
            *status = CHARGED;
        } else {
            *status = CHARGING;
        }
    }

    return TRUE;
}

/*
 * computation functions
 */

static gboolean get_battery_charge (apm_info *info, gboolean remaining, gint *percentage, gint *time)
{
    g_return_val_if_fail (percentage != NULL, FALSE);

    *percentage = info->battery_percentage;

    if (time == NULL) {
        return TRUE;
    }

    if (remaining == FALSE) {
        return get_battery_time_estimation (info->battery_percentage, 100, time);
    }

    if (info->using_minutes) {
        *time = info->battery_time;
    } else {
        *time = (info->battery_time + 30) / 60;
    }

    return TRUE;
}

static gboolean get_battery_time_estimation (gdouble remaining_capacity, gdouble y, gint *time)
{
    if (estimation_remaining_capacity == -1) {
        estimation_remaining_capacity = remaining_capacity;
    }

    /*
     * y = mx + b ... x = (y - b) / m
     * solving for when y = 0 (discharging) or full_capacity (charging)
     */

    if (remaining_capacity != estimation_remaining_capacity) {
        gdouble estimation_elapsed = g_timer_elapsed (estimation_timer, NULL);
        gdouble estimation_current_rate = (remaining_capacity - estimation_remaining_capacity) / estimation_elapsed;
        gdouble estimation_seconds = (y - remaining_capacity) / estimation_current_rate;

        *time = (gint)(estimation_seconds / 60.0);

        estimation_remaining_capacity = remaining_capacity;
        estimation_time               = *time;
        g_timer_start (estimation_timer);
    } else {
        *time = estimation_time;
    }

    return TRUE;
}

static void reset_battery_time_estimation (void)
{
    estimation_remaining_capacity = -1;
    estimation_time               = -1;
    g_timer_start (estimation_timer);
}

/*
 * tray icon functions
 */

static void create_tray_icon (void)
{
    struct icon* tray_icon = g_malloc (sizeof(*tray_icon));
    tray_icon->egg_tray_icon = egg_tray_icon_new (CBATTICON_STRING);
    tray_icon->image = gtk_image_new ();
    tray_icon->tooltips = gtk_tooltips_new ();
    tray_icon->name = g_strdup ("");
    tray_icon->size = 24;

    gtk_tooltips_set_tip (GTK_TOOLTIPS (tray_icon->tooltips), GTK_WIDGET (tray_icon->egg_tray_icon), CBATTICON_STRING, "");

    /* If the system tray goes away, our icon will get destroyed,
        * and we don't want to be left with a dangling pointer to it
        * if that happens.  */
    g_object_add_weak_pointer (G_OBJECT(tray_icon->egg_tray_icon), (void**)&tray_icon->egg_tray_icon);
    g_object_add_weak_pointer (G_OBJECT(tray_icon->image), (void**)&tray_icon->image);

    /* Add the image to the icon. */
    gtk_container_add (GTK_CONTAINER(tray_icon->egg_tray_icon), tray_icon->image);
    gtk_widget_show (tray_icon->image);

    update_tray_icon (tray_icon);
    g_timeout_add (configuration.update_interval * 1000, (GSourceFunc)update_tray_icon, (gpointer)tray_icon);

    /* Handle clicking events. */
    gtk_widget_add_events (GTK_WIDGET (tray_icon->egg_tray_icon), GDK_BUTTON_PRESS_MASK);
    g_signal_connect (G_OBJECT (tray_icon->egg_tray_icon), "button_press_event", G_CALLBACK (on_tray_icon_click), NULL);

    gtk_widget_show(GTK_WIDGET (tray_icon->egg_tray_icon));
}

static void hash_table_free (gpointer data)
{
    free (data);
}

static void set_tray_icon (struct icon *tray_icon, const gchar *name)
{
    /*gint size = gtk_status_icon_get_size (tray_icon->gtk_icon);

    if (size == tray_icon->size && (name == NULL || g_strcmp0 (name, tray_icon->name) == 0)) {
        return;
    }

    tray_icon->size = size;*/

    if (name != NULL)
    {
        g_free (tray_icon->name);
        tray_icon->name = g_strdup (name);
    }

    if (icons_cache == NULL) {
        icons_cache = g_hash_table_new_full (g_str_hash, g_str_equal, hash_table_free, hash_table_free);
    }

    gpointer value = g_hash_table_lookup (icons_cache, name);

    if (value == NULL) {
        value = gdk_pixbuf_new_from_file (get_icon_path(name), NULL);
        g_assert (value != NULL);
        g_hash_table_insert (icons_cache, g_strdup (name), value);
    }

    gtk_image_set_from_pixbuf (GTK_IMAGE(tray_icon->image), GDK_PIXBUF(value));
}

static gboolean update_tray_icon (struct icon *tray_icon)
{
    g_return_val_if_fail (tray_icon != NULL, FALSE);

    update_tray_icon_status (tray_icon);

    return TRUE;
}

static void set_tooltip_text (struct icon *tray_icon, const gchar *tip_text)
{
    gtk_tooltips_set_tip (GTK_TOOLTIPS (tray_icon->tooltips), GTK_WIDGET (tray_icon->egg_tray_icon), tip_text, "");
}

static void update_tray_icon_status (struct icon *tray_icon)
{
    GError *error = NULL;

    gint battery_status            = -1;
    static gint old_battery_status = -1;

    /* battery statuses:                                      */
    /* not present => ac_only, battery_missing                */
    /* present     => charging, charged, discharging, unknown */
    /* (present and not present are exclusive)                */

    /*static gboolean ac_only                = FALSE;*/
    static gboolean battery_low            = FALSE;
    static gboolean battery_critical       = FALSE;
    static gboolean spawn_command_low      = FALSE;
    static gboolean spawn_command_critical = FALSE;

    gint percentage, time;
    gchar *battery_string, *time_string;

#ifdef WITH_NOTIFY
    static NotifyNotification *notification = NULL;
#endif

    apm_info info;

    apm_read (&info);

    /* update tray icon for AC only */

    /* TODO: detect this state */
    /*if (battery_path == NULL) {
        if (ac_only == FALSE) {
            ac_only = TRUE;

            NOTIFY_MESSAGE (&notification, _("AC only, no battery!"), NULL, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL);

            set_tooltip_text (tray_icon, _("AC only, no battery!"));

            if (configuration.icon_type == BATTERY_ICON_GPM) {
                set_tray_icon (tray_icon, "gpm-ac-adapter");
            } else {
                set_tray_icon (tray_icon, "ac-adapter");
            }
        }

        return;
    }*/

    /* update tray icon for battery */

    if (get_battery_status (&info, &battery_status) == FALSE) {
        return;
    }

    #define HANDLE_BATTERY_STATUS(PCT,TIM,EXP,URG)                                                          \
                                                                                                            \
            percentage = PCT;                                                                               \
                                                                                                            \
            battery_string = get_battery_string (battery_status, percentage);                               \
            time_string    = get_time_string (TIM);                                                         \
                                                                                                            \
            if (old_battery_status != battery_status) {                                                     \
                old_battery_status  = battery_status;                                                       \
                NOTIFY_MESSAGE (&notification, battery_string, time_string, EXP, URG);                      \
            }                                                                                               \
                                                                                                            \
            set_tooltip_text (tray_icon, get_tooltip_string (battery_string, time_string)); \
            set_tray_icon (tray_icon, get_icon_name (battery_status, percentage));

    switch (battery_status) {
        case MISSING:
            HANDLE_BATTERY_STATUS (0, -1, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL)
            break;

        case UNKNOWN:
            HANDLE_BATTERY_STATUS (0, -1, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case CHARGED:
            HANDLE_BATTERY_STATUS (100, -1, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case CHARGING:
            if (old_battery_status != CHARGING) {
                reset_battery_time_estimation ();
            }

            if (get_battery_charge (&info, FALSE, &percentage, &time) == FALSE) {
                return;
            }

            HANDLE_BATTERY_STATUS (percentage, time, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case DISCHARGING:
        case NOTCHARGING:
            if (get_battery_charge (&info, TRUE, &percentage, &time) == FALSE) {
                return;
            }

            battery_string = get_battery_string (battery_status, percentage);
            time_string    = get_time_string (time);

            if (old_battery_status != DISCHARGING) {
                old_battery_status  = DISCHARGING;
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL);

                battery_low            = FALSE;
                battery_critical       = FALSE;
                spawn_command_low      = FALSE;
                spawn_command_critical = FALSE;
            }

            if (battery_low == FALSE && percentage <= configuration.low_level) {
                battery_low = TRUE;

                battery_string = get_battery_string (LOW_LEVEL, percentage);
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL);

                spawn_command_low = TRUE;
            }

            if (battery_critical == FALSE && percentage <= configuration.critical_level) {
                battery_critical = TRUE;

                battery_string = get_battery_string (CRITICAL_LEVEL, percentage);
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);

                spawn_command_critical = TRUE;
            }

            set_tooltip_text (tray_icon, get_tooltip_string (battery_string, time_string));
            set_tray_icon (tray_icon, get_icon_name (battery_status, percentage));

            if (spawn_command_low == TRUE) {
                spawn_command_low = FALSE;

                if (configuration.command_low_level != NULL) {
                    syslog (LOG_CRIT, _("Spawning low battery level command in 5 seconds: %s"), configuration.command_low_level);
                    g_usleep (G_USEC_PER_SEC * 5);

                    if (get_battery_status (&info, &battery_status) == TRUE) {
                        if (battery_status != DISCHARGING && battery_status != NOTCHARGING) {
                            syslog (LOG_NOTICE, _("Skipping low battery level command, no longer discharging"));
                            return;
                        }
                    }

                    if (g_spawn_command_line_async (configuration.command_low_level, &error) == FALSE) {
                        syslog (LOG_CRIT, _("Cannot spawn low battery level command: %s\n"), error->message);

                        g_printerr (_("Cannot spawn low battery level command: %s\n"), error->message);
                        g_error_free (error); error = NULL;

#ifdef WITH_NOTIFY
                        static NotifyNotification *spawn_notification = NULL;
                        NOTIFY_MESSAGE (&spawn_notification, _("Cannot spawn low battery level command!"), configuration.command_low_level, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);
#endif
                    }
                }
            }

            if (spawn_command_critical == TRUE) {
                spawn_command_critical = FALSE;

                if (configuration.command_critical_level != NULL) {
                    syslog (LOG_CRIT, _("Spawning critical battery level command in 30 seconds: %s"), configuration.command_critical_level);
                    g_usleep (G_USEC_PER_SEC * 30);

                    if (get_battery_status (&info, &battery_status) == TRUE) {
                        if (battery_status != DISCHARGING && battery_status != NOTCHARGING) {
                            syslog (LOG_NOTICE, _("Skipping critical battery level command, no longer discharging"));
                            return;
                        }
                    }

                    if (g_spawn_command_line_async (configuration.command_critical_level, &error) == FALSE) {
                        syslog (LOG_CRIT, _("Cannot spawn critical battery level command: %s\n"), error->message);

                        g_printerr (_("Cannot spawn critical battery level command: %s\n"), error->message);
                        g_error_free (error); error = NULL;

#ifdef WITH_NOTIFY
                        static NotifyNotification *spawn_notification = NULL;
                        NOTIFY_MESSAGE (&spawn_notification, _("Cannot spawn critical battery level command!"), configuration.command_critical_level, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);
#endif
                    }
                }
            }
            break;
    }
}

static gboolean on_tray_icon_click (struct icon *tray_icon, GdkEventButton *event, gpointer user_data)
{
    GError *error = NULL;

    if (event->button != 1) {
        return FALSE;
    }

    if (configuration.command_left_click != NULL) {
        if (g_spawn_command_line_async (configuration.command_left_click, &error) == FALSE) {
            syslog (LOG_ERR, _("Cannot spawn left click command: %s\n"), error->message);

            g_printerr (_("Cannot spawn left click command: %s\n"), error->message);
            g_error_free (error); error = NULL;

#ifdef WITH_NOTIFY
            static NotifyNotification *spawn_notification = NULL;
            NOTIFY_MESSAGE (&spawn_notification, _("Cannot spawn left click command!"), configuration.command_left_click, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_CRITICAL);
#endif
        }
    }

    return TRUE;
}

#ifdef WITH_NOTIFY
static void notify_message (NotifyNotification **notification, gchar *summary, gchar *body, gint timeout, NotifyUrgency urgency)
{
    g_return_if_fail (notification != NULL);
    g_return_if_fail (summary != NULL);

    if (configuration.hide_notification == TRUE) {
        return;
    }

    if (*notification == NULL) {
#if NOTIFY_CHECK_VERSION (0, 7, 0)
        *notification = notify_notification_new (summary, body, NULL);
#else
        *notification = notify_notification_new (summary, body, NULL, NULL);
#endif
    } else {
        notify_notification_update (*notification, summary, body, NULL);
    }

    notify_notification_set_timeout (*notification, timeout);
    notify_notification_set_urgency (*notification, urgency);
    notify_notification_show (*notification, NULL);
}
#endif

static gchar* get_tooltip_string (gchar *battery, gchar *time)
{
    static gchar tooltip_string[STR_LTH];

    tooltip_string[0] = '\0';

    g_return_val_if_fail (battery != NULL, tooltip_string);

    g_strlcpy (tooltip_string, battery, STR_LTH);

    if (configuration.debug_output == TRUE) {
        g_printf ("tooltip: %s\n", battery);
    }

    if (time != NULL) {
        g_strlcat (tooltip_string, "\n", STR_LTH);
        g_strlcat (tooltip_string, time, STR_LTH);

        if (configuration.debug_output == TRUE) {
            g_printf ("tooltip: %s\n", time);
        }
    }

    return tooltip_string;
}

static gchar* get_battery_string (gint state, gint percentage)
{
    static gchar battery_string[STR_LTH];

    switch (state) {
        case MISSING:
            g_strlcpy (battery_string, _("Battery is missing!"), STR_LTH);
            break;

        case UNKNOWN:
            g_strlcpy (battery_string, _("Battery status is unknown!"), STR_LTH);
            break;

        case CHARGED:
            g_strlcpy (battery_string, _("Battery is charged!"), STR_LTH);
            break;

        case DISCHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is discharging (%i%% remaining)"), percentage);
            break;

        case NOTCHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is not charging (%i%% remaining)"), percentage);
            break;

        case LOW_LEVEL:
            g_snprintf (battery_string, STR_LTH, _("Battery level is low! (%i%% remaining)"), percentage);
            break;

        case CRITICAL_LEVEL:
            g_snprintf (battery_string, STR_LTH, _("Battery level is critical! (%i%% remaining)"), percentage);
            break;

        case CHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is charging (%i%%)"), percentage);
            break;

        default:
            battery_string[0] = '\0';
            break;
    }

    if (configuration.debug_output == TRUE) {
        g_printf ("battery string: %s\n", battery_string);
    }

    return battery_string;
}

static gchar* get_time_string (gint minutes)
{
    static gchar time_string[STR_LTH];
    static gchar minutes_string[STR_LTH];
    gint hours;

    if (minutes < 0) {
        return NULL;
    }

    hours   = minutes / 60;
    minutes = minutes % 60;

    if (hours > 0) {
        g_sprintf (minutes_string, g_dngettext (NULL, "%d minute", "%d minutes", minutes), minutes);
        g_sprintf (time_string, g_dngettext (NULL, "%d hour, %s remaining", "%d hours, %s remaining", hours), hours, minutes_string);
    } else {
        g_sprintf (time_string, g_dngettext (NULL, "%d minute remaining", "%d minutes remaining", minutes), minutes);
    }

    if (configuration.debug_output == TRUE) {
        g_printf ("time string: %s\n", time_string);
    }

    return time_string;
}

static gchar* get_icon_name (gint state, gint percentage)
{
    static gchar icon_name[STR_LTH];

    if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
        g_strlcpy (icon_name, "notification-battery", STR_LTH);
    } else if (configuration.icon_type == BATTERY_ICON_GPM) {
        g_strlcpy (icon_name, "gpm-primary", STR_LTH);
    } else {
        g_strlcpy (icon_name, "battery", STR_LTH);
    }

    if (state == MISSING || state == UNKNOWN) {
        if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
            g_strlcat (icon_name, "-empty", STR_LTH);
        } else {
            g_strlcat (icon_name, "-missing", STR_LTH);
        }
    } else {
        if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
                 if (percentage <= 20)  g_strlcat (icon_name, "-020", STR_LTH);
            else if (percentage <= 40)  g_strlcat (icon_name, "-040", STR_LTH);
            else if (percentage <= 60)  g_strlcat (icon_name, "-060", STR_LTH);
            else if (percentage <= 80)  g_strlcat (icon_name, "-080", STR_LTH);
            else                        g_strlcat (icon_name, "-100", STR_LTH);

                 if (state == CHARGING) g_strlcat (icon_name, "-plugged", STR_LTH);
            else if (state == CHARGED)  g_strlcat (icon_name, "-plugged", STR_LTH);
        } else if (configuration.icon_type == BATTERY_ICON_GPM) {
                 if (state == CHARGED)  g_strlcat (icon_name, "-charged", STR_LTH);
            else if (percentage <= 20)  g_strlcat (icon_name, "-020", STR_LTH);
            else if (percentage <= 40)  g_strlcat (icon_name, "-040", STR_LTH);
            else if (percentage <= 60)  g_strlcat (icon_name, "-060", STR_LTH);
            else if (percentage <= 80)  g_strlcat (icon_name, "-080", STR_LTH);
            else                        g_strlcat (icon_name, "-100", STR_LTH);

                 if (state == CHARGING) g_strlcat (icon_name, "-charging", STR_LTH);
        } else {
                 if (percentage <= 20)  g_strlcat (icon_name, "-caution", STR_LTH);
            else if (percentage <= 40)  g_strlcat (icon_name, "-low", STR_LTH);
            else if (percentage <= 80)  g_strlcat (icon_name, "-good", STR_LTH);
            else                        g_strlcat (icon_name, "-full", STR_LTH);

                 if (state == CHARGING) g_strlcat (icon_name, "-charging", STR_LTH);
            else if (state == CHARGED)  g_strlcat (icon_name, "-charged", STR_LTH);
        }
    }

    if (configuration.debug_output == TRUE) {
        g_printf ("icon name: %s\n", icon_name);
    }

    return icon_name;
}

int main (int argc, char **argv)
{
    gint ret;

    switch (apm_exists ()) {
        case 1:
            fprintf (stderr, "No APM support in kernel\n");
            exit (1);
        case 2:
            fprintf (stderr, "Old APM support in kernel\n");
            exit (2);
    }

    setlocale (LC_ALL, "");
    bindtextdomain (CBATTICON_STRING, NLSDIR);
    bind_textdomain_codeset (CBATTICON_STRING, "UTF-8");
    textdomain (CBATTICON_STRING);

    ret = get_options (argc, argv);
    if (ret <= 0) {
        return ret;
    }

#ifdef WITH_NOTIFY
    if (configuration.hide_notification == FALSE) {
        if (notify_init (CBATTICON_STRING) == FALSE) {
            return -1;
        }
    }
#endif

    estimation_timer = g_timer_new ();

    create_tray_icon ();
    gtk_main();

    return 0;
}
