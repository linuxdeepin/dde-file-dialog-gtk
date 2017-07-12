#include <gtk/gtk.h>
#include <gtk/gtkfilechooserprivate.h>
#include <gtk/gtkfilefilter.h>
#include <gdk/x11/gdkx.h>

#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>

#define D_STRINGIFY(x) #x

#define DFM_FILEDIALOG_DBUS_SERVER "com.deepin.filemanager.filedialog"
#define DFM_FILEDIALOGMANAGER_DBUS_PATH "/com/deepin/filemanager/filedialogmanager"
#define DFM_FILEDIALOGMANAGER_DBUS_INTERFACE "com.deepin.filemanager.filedialogmanager"
#define DFM_FILEDIALOG_DBUS_INTERFACE "com.deepin.filemanager.filedialog"

#ifndef DISABLE_DEBUG
#define d_debug(...) if (getenv("_d_enable_filedialog_debug")) { printf("In Line: %d, In Function: %s: ", __LINE__, G_STRFUNC); printf(__VA_ARGS__);}
#else
#define d_debug(format, ...)
#endif

typedef enum {
    AnyFile = 0,
    ExistingFile = 1,
    Directory = 2,
    ExistingFiles = 3,
    DirectoryOnly = 4
} FileMode;

typedef enum {
    AcceptOpen = 0,
    AcceptSave = 1
} AcceptMode;

typedef enum {
    Rejected = 0,
    Accepted = 1
} DialogCode;

typedef enum {
    ShowDirsOnly                = 0x00000001,
    DontResolveSymlinks         = 0x00000002,
    DontConfirmOverwrite        = 0x00000004,
    DontUseSheet                = 0x00000008,
    DontUseNativeDialog         = 0x00000010,
    ReadOnly                    = 0x00000020,
    HideNameFilterDetails       = 0x00000040,
    DontUseCustomDirectoryIcons = 0x00000080
} Option;

//! The code from libgtk+2.0 source
struct _GtkFileFilter
{
  GtkObject parent_instance;

  gchar *name;
  GSList *rules;

  GtkFileFilterFlags needed;
};

typedef enum {
  FILTER_RULE_PATTERN,
  FILTER_RULE_MIME_TYPE,
  FILTER_RULE_PIXBUF_FORMATS,
  FILTER_RULE_CUSTOM
} FilterRuleType;

typedef struct _FilterRule
{
  FilterRuleType type;
  GtkFileFilterFlags needed;

  union {
    gchar *pattern;
    gchar *mime_type;
    GSList *pixbuf_formats;
    struct {
      GtkFileFilterFunc func;
      gpointer data;
      GDestroyNotify notify;
    } custom;
  } u;
} FilterRule;

static GtkWidget *
gtk_file_chooser_dialog_new_valist(const gchar              *title,
                                   GtkWindow                *parent,
                                   GtkFileChooserAction     action,
                                   const gchar              *backend,
                                   const gchar              *first_button_text,
                                   va_list                  varargs)
{
    (void)backend;

    GtkWidget *result;
    const char *button_text = first_button_text;
    gint response_id;

    result = g_object_new (GTK_TYPE_FILE_CHOOSER_DIALOG,
                           "title", title,
                           "action", action,
                           NULL);

    if (parent)
        gtk_window_set_transient_for (GTK_WINDOW (result), parent);

    while (button_text)
    {
        response_id = va_arg (varargs, gint);
        gtk_dialog_add_button (GTK_DIALOG (result), button_text, response_id);
        d_debug("button text: %s, response id: %d\n", button_text, response_id);
        button_text = va_arg (varargs, const gchar *);
    }

    return result;
}

static GSList *
files_to_strings (GSList  *files,
          gchar * (*convert_func) (GFile *file))
{
  GSList *strings;

  strings = NULL;

  for (; files; files = files->next)
    {
      GFile *file;
      gchar *string;

      file = files->data;
      string = (* convert_func) (file);

      if (string)
    strings = g_slist_prepend (strings, string);
    }

  return g_slist_reverse (strings);
}

static gchar *
file_to_uri_with_native_path (GFile *file)
{
  gchar *result = NULL;
  gchar *native;

  native = g_file_get_path (file);
  if (native)
    {
      result = g_filename_to_uri (native, NULL, NULL); /* NULL-GError */
      g_free (native);
    }

  return result;
}
//! End

static GDBusConnection *
get_connection (void)
{
    static GDBusConnection *connection = NULL;
    GError *error = NULL;

    if (connection)
        return connection;

    /* Normally I hate sync calls with UIs, but we need to return NULL
   * or a GtkSearchEngine as a result of this function.
   */
    connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

    if (error)
    {
        g_debug ("Couldn't connect to D-Bus session bus, %s", error->message);
        g_error_free (error);
        return NULL;
    }

    /* If connection is set, we know it worked. */
    g_debug ("Finding out if Tracker is available via D-Bus...");

    return connection;
}

static GVariant *d_dbus_filedialog_call_sync(const gchar        *object_path,
                                             const gchar        *interface_name,
                                             const gchar        *method_name,
                                             GVariant           *parameters,
                                             const GVariantType *reply_type,
                                             GVariant           **reply_data)
{
    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return FALSE;
    }

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(dbus_connection,
                                                   DFM_FILEDIALOG_DBUS_SERVER,
                                                   object_path,
                                                   interface_name,
                                                   method_name,
                                                   parameters,
                                                   reply_type,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   5 * 1000,
                                                   NULL,
                                                   &error);

    if (error) {
        g_warning("Call \"%s\" is failed, %s", method_name, error->message);
        g_error_free(error);

        return FALSE;
    }

    *reply_data =  result;

    return TRUE;
}

static gboolean d_dbus_filedialog_call_by_ghost_widget_sync(GtkWidget       *widget_ghost,
                                                            const gchar     *method_name,
                                                            GVariant        *parameters,
                                                            const gchar     *reply_type,
                                                            gpointer        reply_data)
{
    gchar *dbus_object_path = g_object_get_data(GTK_OBJECT(widget_ghost), D_STRINGIFY(_d_dbus_file_dialog_object_path));

    if (!dbus_object_path)
        return FALSE;

    GVariant *result = NULL;
    gboolean ok = d_dbus_filedialog_call_sync(dbus_object_path,
                                              DFM_FILEDIALOG_DBUS_INTERFACE,
                                              method_name,
                                              parameters,
                                              (const GVariantType *)reply_type,
                                              &result);

    if (reply_data)
        g_variant_get(result, reply_type, reply_data);

    if (result) {
        const gchar *tmp_variant_print = g_variant_print(result, TRUE);
        const gchar *tmp_variant_type_string = g_variant_get_type_string(result);

        d_debug("%s, type=%s\n", tmp_variant_print, tmp_variant_type_string);
        g_free(tmp_variant_print);
        g_variant_unref(result);
    }

    return ok;
}

static gboolean d_dbus_filedialogmanager_call_by_ghost_widget_sync(const gchar     *method_name,
                                                                   GVariant        *parameters,
                                                                   const gchar     *reply_type,
                                                                   gpointer        reply_data)
{
    GVariant *result = NULL;
    gboolean ok = d_dbus_filedialog_call_sync(DFM_FILEDIALOGMANAGER_DBUS_PATH,
                                              DFM_FILEDIALOGMANAGER_DBUS_INTERFACE,
                                              method_name,
                                              parameters,
                                              (const GVariantType *)reply_type,
                                              &result);

    if (reply_data)
        g_variant_get(result, reply_type, reply_data);

    if (result) {
        const gchar *tmp_variant_print = g_variant_print(result, TRUE);
        const gchar *tmp_variant_type_string = g_variant_get_type_string(result);

        d_debug("%s, type=%s\n", tmp_variant_print, tmp_variant_type_string);
        g_free(tmp_variant_print);
        g_variant_unref(result);
    }

    return ok;
}

static gboolean d_dbus_filedialog_get_property_by_ghost_widget_sync(GtkWidget   *widget_ghost,
                                                                    const gchar *property_name,
                                                                    const gchar *reply_type,
                                                                    gpointer    reply_data)
{
    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return FALSE;
    }

    gchar *dbus_object_path = g_object_get_data(GTK_OBJECT(widget_ghost), D_STRINGIFY(_d_dbus_file_dialog_object_path));

    if (!dbus_object_path) {
        return FALSE;
    }

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(dbus_connection,
                                                   DFM_FILEDIALOG_DBUS_SERVER,
                                                   dbus_object_path,
                                                   "org.freedesktop.DBus.Properties",
                                                   "Get",
                                                   g_variant_new("(ss)", DFM_FILEDIALOG_DBUS_INTERFACE, property_name),
                                                   "(v)",
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   5 * 1000,
                                                   NULL,
                                                   &error);

    if (error) {
        g_warning("Get property \"%s\" is failed, %s", property_name, error->message);
        g_error_free(error);

        return FALSE;
    }

    GVariant *old_result = result;

    g_variant_get(old_result, "(v)", &result);
    g_variant_unref(old_result);

    if (reply_data)
        g_variant_get(result, reply_type, reply_data);

    if (result) {
        const gchar *tmp_variant_print = g_variant_print(result, TRUE);
        const gchar *tmp_variant_type_string = g_variant_get_type_string(result);

        d_debug("%s, type=%s\n", tmp_variant_print, tmp_variant_type_string);
        g_free(tmp_variant_print);
        g_variant_unref(result);
    }

    return TRUE;
}

static gboolean d_dbus_filedialog_set_property_by_ghost_widget_sync(GtkWidget   *widget_ghost,
                                                                    const gchar *property_name,
                                                                    GVariant    *value)
{
    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return FALSE;
    }

    gchar *dbus_object_path = g_object_get_data(GTK_OBJECT(widget_ghost), D_STRINGIFY(_d_dbus_file_dialog_object_path));

    if (!dbus_object_path) {
        return FALSE;
    }

    GError *error = NULL;
    g_dbus_connection_call_sync(dbus_connection,
                                DFM_FILEDIALOG_DBUS_SERVER,
                                dbus_object_path,
                                "org.freedesktop.DBus.Properties",
                                "Set",
                                g_variant_new("(ssv)", DFM_FILEDIALOG_DBUS_INTERFACE, property_name, value),
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                5 * 1000,
                                NULL,
                                &error);

    if (error) {
        g_warning("Set property \"%s\" is failed, %s", property_name, error->message);
        g_error_free(error);

        return FALSE;
    }

    return TRUE;
}

static guint d_dbus_filedialog_connection_signal(const gchar            *object_path,
                                                 const gchar            *signal_name,
                                                 GDBusSignalCallback    callback,
                                                 gpointer               user_data)
{
    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return -1;
    }

    return g_dbus_connection_signal_subscribe(dbus_connection,
                                              DFM_FILEDIALOG_DBUS_SERVER,
                                              DFM_FILEDIALOG_DBUS_INTERFACE,
                                              signal_name,
                                              object_path,
                                              NULL,
                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                              callback,
                                              user_data,
                                              NULL);
}

static void d_show_dbus_filedialog(GtkWidget *widget_ghost)
{
    if (!d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost, "show", NULL, NULL, NULL))
        return;

    guint64 dbus_dialog_winId;

    if (!d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost, "winId", NULL, "(t)", &dbus_dialog_winId))
        return;

    XSetTransientForHint(gdk_x11_get_default_xdisplay(), dbus_dialog_winId, GDK_WINDOW_XID(gtk_widget_get_window(widget_ghost)));
}

static void d_hide_dbus_filedialog(GtkWidget *widget_ghost)
{
    d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost, "hide", NULL, NULL, NULL);
}

static void d_on_dbus_filedialog_finished(GDBusConnection  *connection,
                                           const gchar      *sender_name,
                                           const gchar      *object_path,
                                           const gchar      *interface_name,
                                           const gchar      *signal_name,
                                           GVariant         *parameters,
                                           GtkWidget        *widget_ghost)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    gint32 dialog_code;
    g_variant_get(parameters, "(i)", &dialog_code);

    if (dialog_code == Accepted && gtk_file_chooser_get_action(widget_ghost) == GTK_FILE_CHOOSER_ACTION_SAVE) {
        GFile *file = gtk_file_chooser_get_file(widget_ghost);

        if (file) {
            char *file_basename = g_file_get_basename(file);
            g_object_unref(file);
            gtk_file_chooser_set_current_name(widget_ghost, file_basename);
            free(file_basename);
        }
    }

    gint accept_response_id ,reject_response_id;

    d_get_gtk_dialog_response_id(GTK_DIALOG(widget_ghost), &accept_response_id, &reject_response_id);
    gtk_dialog_response(GTK_DIALOG(widget_ghost), dialog_code == Rejected ? reject_response_id : accept_response_id);

    d_debug("accept response id: %d, reject response id: %d\n", accept_response_id, reject_response_id);
}

static void d_on_dbus_filedialog_selectionFilesChanged(GDBusConnection  *connection,
                                                        const gchar      *sender_name,
                                                        const gchar      *object_path,
                                                        const gchar      *interface_name,
                                                        const gchar      *signal_name,
                                                        GVariant         *parameters,
                                                        GtkWidget        *widget_ghost)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;

    GVariantIter *selected_files = NULL;
    gboolean ok = d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost,
                                                              "selectedUrls",
                                                              NULL,
                                                              "(as)",
                                                              &selected_files);

    if (!ok || !selected_files)
        return;

    int selected_files_length = g_variant_iter_n_children(selected_files);

    for (int i = 0; i < selected_files_length; ++i) {
        const gchar *file_uri = g_variant_get_string(g_variant_iter_next_value(selected_files), NULL);

        gtk_file_chooser_select_uri(GTK_FILE_CHOOSER_DIALOG(widget_ghost), file_uri);
        d_debug("selected file uri=%s\n", file_uri);
    }

    g_variant_iter_free(selected_files);
}

static void d_on_dbus_filedialog_currentUrlChanged(GDBusConnection  *connection,
                                                   const gchar      *sender_name,
                                                   const gchar      *object_path,
                                                   const gchar      *interface_name,
                                                   const gchar      *signal_name,
                                                   GVariant         *parameters,
                                                   GtkWidget        *widget_ghost)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;

    gchar *current_url = NULL;
    gboolean ok = d_dbus_filedialog_get_property_by_ghost_widget_sync(widget_ghost,
                                                                      "directoryUrl",
                                                                      "s",
                                                                      &current_url);

    if (!ok || !current_url)
        return;

    d_debug("dbus file dialog current uri: %s\n", current_url);

    GFile *file = g_file_new_for_uri (current_url);
    GTK_FILE_CHOOSER_GET_IFACE (GTK_FILE_CHOOSER(widget_ghost))->set_current_folder (GTK_FILE_CHOOSER(widget_ghost), file, NULL);
    g_object_unref (file);
    g_free(current_url);
}

static void d_on_gtk_filedialog_destroy(GtkWidget *object,
                                        gpointer   user_data)
{
    (void)object;
    (void)user_data;

    guint timeout_handler_id = g_object_get_data(object, D_STRINGIFY(_d_dbus_file_dialog_heartbeat_timer_handler_id));
    gtk_timeout_remove(timeout_handler_id);

    d_debug("d_on_gtk_filedialog_destroy\n");
}

static void d_heartbeat_filedialog(GtkWidget *widget_ghost)
{
    gboolean ok = d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost,
                                                              "makeHeartbeat",
                                                              NULL, NULL, NULL);
    if (!ok)
        d_debug("The dbus file dialog has beed die\n");

    if (!ok) {
        gint rejept_response_id;
        d_get_gtk_dialog_response_id(GTK_DIALOG(widget_ghost), NULL, &rejept_response_id);
        gtk_dialog_response(GTK_DIALOG(widget_ghost), rejept_response_id);
    }
}

static int d_bbyte_array_find_char(const GByteArray *array, gchar ch, int start)
{
    for (; start < array->len; ++start) {
        if ((gchar)array->data[start] == ch)
            return start;
    }

    return -1;
}

static GByteArray *d_gtk_file_filter_to_string(const GtkFileFilter *filter)
{
    if (filter->needed & (GTK_FILE_FILTER_FILENAME | GTK_FILE_FILTER_DISPLAY_NAME | GTK_FILE_FILTER_MIME_TYPE) == 0) {
        return NULL;
    }

    GByteArray *byte_array = g_byte_array_new();

    if (filter->name) {
        g_byte_array_append(byte_array, filter->name, strlen(filter->name));
        d_debug("name: %s\n", filter->name);
    }

    if (!filter->rules)
        return byte_array;

    int rule_list_length = g_slist_length(filter->rules);

    d_debug("rule list length: %d\n", rule_list_length);

    if (rule_list_length <= 0)
        return byte_array;

    int left_parenthesis_index = d_bbyte_array_find_char(byte_array, '(', 0);

    if (left_parenthesis_index > 0 && (char)byte_array->data[byte_array->len - 1] == ')') {
        g_byte_array_remove_range(byte_array, left_parenthesis_index, byte_array->len - left_parenthesis_index + 1);
    }

    g_byte_array_append(byte_array, " (", 2);

    for (int i = 0; i < rule_list_length; ++i) {
        FilterRule *rule = g_slist_nth_data(filter->rules, i);

        if (rule->type == FILTER_RULE_PATTERN) {
            d_debug("%d: pattern: %s\n", i, rule->u.pattern);

            if (rule->u.pattern && strlen(rule->u.pattern) > 0) {
                int length = strlen(rule->u.pattern);
                gchar *new_pattern = (gchar*)malloc(sizeof(gchar) * length);

                for (int i = 0; i < length; ++i) {
                    if (rule->u.pattern[i] == '(' || rule->u.pattern[i] == ')')
                        new_pattern[i] = ' ';
                    else
                        new_pattern[i] = rule->u.pattern[i];
                }

                g_byte_array_append(byte_array, new_pattern, length);
                g_byte_array_append(byte_array, " ", 1);

                free(new_pattern);
            }
        } else if (rule->type == FILTER_RULE_MIME_TYPE) {
            GVariantIter *patterns = NULL;
            gboolean ok = d_dbus_filedialogmanager_call_by_ghost_widget_sync("globPatternsForMime",
                                                                             g_variant_new("(s)", rule->u.mime_type),
                                                                             "(as)",
                                                                             &patterns);
            d_debug("%d: ok: %d, patterns: %lx\n", i, ok, patterns);

            if (!ok || !patterns) {
                g_byte_array_append(byte_array, rule->u.mime_type, strlen(rule->u.mime_type));
                g_byte_array_append(byte_array, " ", 1);
                continue;
            }

            int mimes_length = g_variant_iter_n_children(patterns);

            d_debug("mimes length: %d\n", mimes_length);

            for (int j = 0; j < mimes_length; ++j) {
                const gchar *pattern = g_variant_get_string(g_variant_iter_next_value(patterns), NULL);

                d_debug("%d: pattern: %s\n", j, pattern);

                g_byte_array_append(byte_array, pattern, strlen(pattern));
                g_byte_array_append(byte_array, " ", 1);
                g_free(pattern);
            }

            g_variant_iter_free(patterns);
        } else {
            d_debug("%d: other rule type: %d\n", i, rule->type);
        }
    }

    if (byte_array->data[byte_array->len - 1] == '(') {
        g_byte_array_free(byte_array, FALSE);

        return NULL;
    }

    g_byte_array_remove_range(byte_array, byte_array->len - 1, 1);
    g_byte_array_append(byte_array, ")", 1);

    return byte_array;
}

static void d_update_filedialog_name_filters(GtkWidget *widget_ghost)
{
    gchar *dbus_object_path = g_object_get_data(GTK_OBJECT(widget_ghost), D_STRINGIFY(_d_dbus_file_dialog_object_path));

    if (!dbus_object_path) {
        return;
    }

    GSList *filter_list = gtk_file_chooser_list_filters(widget_ghost);
    int filter_list_length = g_slist_length(filter_list);

    gchar **list = g_malloc(filter_list_length * sizeof(gchar*));
    int list_length = 0;

    for (int i = 0; i < filter_list_length; ++i) {
        GtkFileFilter *filter = g_slist_nth_data(filter_list, i);
        GByteArray *string = d_gtk_file_filter_to_string(filter);

        if (!string)
            continue;

        g_byte_array_append(string, "\0", 1);
        gchar *str = g_malloc(string->len);
        strcpy(str, string->data);
        list[list_length++] = str;
        d_debug("%d: filter string: %s\n", i, string->data);
        g_byte_array_free(string, FALSE);
    }

    d_debug("%d\n", list_length);

    if (list_length > 0) {
        d_dbus_filedialog_set_property_by_ghost_widget_sync(widget_ghost,
                                                            "nameFilters",
                                                            g_variant_new_strv(list, list_length));
        gtk_file_chooser_set_filter(widget_ghost, gtk_file_chooser_get_filter(widget_ghost));

        for (int i = 0; i < list_length; ++i)
            g_free(list[i]);
    }

    g_free(list);
    g_slist_free(filter_list);
}

static void d_on_filedialog_selected_filter_changed(GDBusConnection  *connection,
                                                    const gchar      *sender_name,
                                                    const gchar      *object_path,
                                                    const gchar      *interface_name,
                                                    const gchar      *signal_name,
                                                    GVariant         *parameters,
                                                    GtkWidget        *widget_ghost)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;

    gchar *selected_filter = NULL;

    gboolean ok = d_dbus_filedialog_call_by_ghost_widget_sync(widget_ghost,
                                                              "selectedNameFilter",
                                                              NULL,
                                                              "(s)",
                                                              &selected_filter);

    if (!ok || !selected_filter) {
        d_debug("Get dbus file dialog selected filter failed!\n");
        return;
    }

    GList *list = gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(widget_ghost));
    int length = g_list_length(list);

    for (int i = 0; i < length; ++i) {
        GtkFileFilter *filter = GTK_FILE_FILTER(g_list_nth_data(list, i));

        if (!filter)
            continue;

        GByteArray *array = d_gtk_file_filter_to_string(filter);

        if (!array)
            continue;

        g_byte_array_append(array, "\0", 1);

        d_debug("%d: %s, %s\n", i, selected_filter, array->data);

        if (strcmp(selected_filter, array->data) == 0) {
            g_object_set (GTK_FILE_CHOOSER(widget_ghost), "filter", filter, NULL);
            g_byte_array_unref(array);
            break;
        }

        g_byte_array_unref(array);
    }

    g_free(selected_filter);
}

void d_get_gtk_dialog_response_id(GtkDialog *dialog, gint *accept_id, gint *reject_id)
{
    if (accept_id) {
        if (gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_OK)) {
            *accept_id = GTK_RESPONSE_OK;
        } else if (gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_YES)) {
            *accept_id = GTK_RESPONSE_YES;
        } else if (gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_APPLY)) {
            *accept_id = GTK_RESPONSE_APPLY;
        } else {
            *accept_id = GTK_RESPONSE_ACCEPT;
        }
    }

    if (reject_id) {
        if (gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_REJECT)) {
            *reject_id = GTK_RESPONSE_REJECT;
        } if (gtk_dialog_get_widget_for_response(dialog, GTK_RESPONSE_NO)) {
            *reject_id = GTK_RESPONSE_NO;
        } else {
            *reject_id = GTK_RESPONSE_CANCEL;
        }
    }
}

GtkWidget *gtk_file_chooser_dialog_new(const gchar          *title,
                                       GtkWindow            *parent,
                                       GtkFileChooserAction  action,
                                       const gchar          *first_button_text,
                                       ...)
{
    d_debug("title: %s, action: %d, first button text: %s\n", title, action, first_button_text);

    va_list varargs;
    va_start (varargs, first_button_text);
    GtkWidget *result = gtk_file_chooser_dialog_new_valist (title, parent, action,
                                                 NULL, first_button_text,
                                                 varargs);
    va_end (varargs);

    if (getenv("_d_disable_filedialog")) {
        return result;
    }

    gboolean enable_dbus_dialog = FALSE;
    gboolean ok = d_dbus_filedialogmanager_call_by_ghost_widget_sync("isUseFileChooserDialog",
                                                                     NULL, "(b)", &enable_dbus_dialog);

    if (ok && !enable_dbus_dialog)
        return result;

    GFile *file_this = g_file_new_for_path(g_get_application_name());

    if (!file_this)
        return result;

    char *file_basename = g_file_get_basename(file_this);
    g_object_unref(file_this);

    ok = d_dbus_filedialogmanager_call_by_ghost_widget_sync("canUseFileChooserDialog",
                                                            g_variant_new("(ss)", "gtk2", file_basename),
                                                            "(b)", &enable_dbus_dialog);
    free(file_basename);

    if (ok && !enable_dbus_dialog)
        return result;

    gchar *dbus_object_path = NULL;
    ok = d_dbus_filedialogmanager_call_by_ghost_widget_sync("createDialog",
                                                            g_variant_new("(s)", ""),
                                                            "(o)",
                                                            &dbus_object_path);

    if (!ok)
        return result;

    gtk_window_set_decorated(GTK_WINDOW(result), FALSE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(result), TRUE);

    d_debug("_d_show_gtk_file_chooser_dialog: %s\n", getenv("_d_show_gtk_file_chooser_dialog"));

    if (!getenv("_d_show_gtk_file_chooser_dialog")) {
        gtk_window_set_opacity(GTK_WINDOW(result), 0);
        char invisible_bitmap_bits[] = { 0x0 };
        GdkBitmap *bitmap = gdk_bitmap_create_from_data(NULL, invisible_bitmap_bits, 1, 1);
        gtk_widget_shape_combine_mask(result, bitmap, 0, 0);

        if (parent) {
            gtk_window_set_accept_focus(GTK_WINDOW(result), FALSE);
            gtk_window_set_transient_for(GTK_WINDOW (result), parent);
            gtk_widget_set_sensitive(result, FALSE);
        }
    }

    g_object_set_data(GTK_OBJECT(result), D_STRINGIFY(_d_dbus_file_dialog_object_path), dbus_object_path);

    // Sync current uri
    gchar *current_folder_uri = gtk_file_chooser_get_current_folder_uri(result);

    if (current_folder_uri) {
        d_debug("gtk dialog current uri: %s\n", current_folder_uri);

        d_dbus_filedialog_set_property_by_ghost_widget_sync(GTK_WIDGET(result),
                                                            "directoryUrl",
                                                            g_variant_new_string(current_folder_uri));
        g_free(current_folder_uri);
    } else {
        d_on_dbus_filedialog_currentUrlChanged(NULL, NULL, NULL, NULL, NULL, NULL, result);
    }

    // GTK widget mapping to DBus file dialog
    g_signal_connect(result, "show", G_CALLBACK(d_show_dbus_filedialog), result);
    g_signal_connect(result, "hide", G_CALLBACK(d_hide_dbus_filedialog), result);

    gtk_file_chooser_set_action(GTK_FILE_CHOOSER(result), action);
    d_dbus_filedialog_call_by_ghost_widget_sync(result,
                                                "setWindowTitle",
                                                g_variant_new("(s)", title),
                                                NULL, NULL);
    d_dbus_filedialog_call_by_ghost_widget_sync(result,
                                                "setOption",
                                                g_variant_new("(ib)", (gint32)HideNameFilterDetails, True),
                                                NULL, NULL);

    // DBus file dialog mapping to GTK widget
    d_dbus_filedialog_connection_signal(dbus_object_path,
                                        "finished",
                                        G_CALLBACK(d_on_dbus_filedialog_finished),
                                        result);
    d_dbus_filedialog_connection_signal(dbus_object_path,
                                        "selectionFilesChanged",
                                        G_CALLBACK(d_on_dbus_filedialog_selectionFilesChanged),
                                        result);

    d_dbus_filedialog_connection_signal(dbus_object_path,
                                        "currentUrlChanged",
                                        G_CALLBACK(d_on_dbus_filedialog_currentUrlChanged),
                                        result);
    d_dbus_filedialog_connection_signal(dbus_object_path,
                                        "selectedNameFilterChanged",
                                        G_CALLBACK(d_on_filedialog_selected_filter_changed),
                                        result);

    // heartbeat for dbus dialog
    int interval = -1;

    if (d_dbus_filedialog_get_property_by_ghost_widget_sync(result,
                                                            "heartbeatInterval",
                                                            "i",
                                                            &interval)) {
        guint timeout_handler_id = gtk_timeout_add(MAX(1 * 1000, MIN((int)(interval / 1.5), interval - 5 * 1000)), d_heartbeat_filedialog, result);
        g_object_set_data(GTK_OBJECT(result), D_STRINGIFY(_d_dbus_file_dialog_heartbeat_timer_handler_id), timeout_handler_id);
        g_signal_connect(result, "destroy", G_CALLBACK(d_on_gtk_filedialog_destroy), NULL);
    }

    return result;
}

void gtk_file_chooser_set_action(GtkFileChooser *chooser, GtkFileChooserAction action)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));

    g_object_set (chooser, "action", action, NULL);

    switch (action) {
    case GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:
        d_dbus_filedialog_call_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                    "setFileMode",
                                                    g_variant_new("(i)", Directory),
                                                    NULL, NULL);
    case GTK_FILE_CHOOSER_ACTION_OPEN:
        d_dbus_filedialog_set_property_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                            "acceptMode",
                                                            g_variant_new_int32(AcceptOpen));

        break;
    case GTK_FILE_CHOOSER_ACTION_SAVE:
        d_dbus_filedialog_set_property_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                            "acceptMode",
                                                            g_variant_new_int32(AcceptSave));

        break;
    default:
        break;
    }
}

//GtkFileChooserAction gtk_file_chooser_get_action(GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_set_local_only(GtkFileChooser *chooser, gboolean local_only)
//{

//}

//gboolean gtk_file_chooser_get_local_only(GtkFileChooser *chooser)
//{

//}

void gtk_file_chooser_set_select_multiple(GtkFileChooser *chooser, gboolean select_multiple)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));

    g_object_set (chooser, "select-multiple", select_multiple, NULL);

    if (gtk_file_chooser_get_action(chooser) == GTK_FILE_CHOOSER_ACTION_OPEN) {
        d_dbus_filedialog_call_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                    "setFileMode",
                                                    g_variant_new("(i)", select_multiple ? ExistingFiles : ExistingFile),
                                                    NULL, NULL);
    }
}

//gboolean gtk_file_chooser_get_select_multiple(GtkFileChooser *chooser)
//{

//}

void gtk_file_chooser_set_show_hidden(GtkFileChooser *chooser, gboolean show_hidden)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));

    g_object_set (chooser, "show-hidden", show_hidden, NULL);
}

//gboolean gtk_file_chooser_get_show_hidden(GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_set_do_overwrite_confirmation(GtkFileChooser *chooser, gboolean do_overwrite_confirmation)
//{

//}

//gboolean gtk_file_chooser_get_do_overwrite_confirmation(GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_set_create_folders(GtkFileChooser *chooser, gboolean create_folders)
//{

//}

//gboolean gtk_file_chooser_get_create_folders(GtkFileChooser *chooser)
//{

//}

/* Suggested name for the Save-type actions
 */

void gtk_file_chooser_set_current_name(GtkFileChooser *chooser, const gchar *name)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));
    g_return_if_fail (name != NULL);

    GTK_FILE_CHOOSER_GET_IFACE (chooser)->set_current_name (chooser, name);

    d_dbus_filedialog_call_by_ghost_widget_sync(chooser,
                                                "setCurrentInputName",
                                                g_variant_new("(s)", name),
                                                NULL, NULL);
}

gchar *gtk_file_chooser_get_filename(GtkFileChooser *chooser)
{
    GFile *file;
    gchar *result = NULL;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    file = gtk_file_chooser_get_file (chooser);

    if (file)
    {
        result = g_file_get_path (file);
        g_object_unref (file);
    }

    return result;
}

//gboolean gtk_file_chooser_select_filename(GtkFileChooser *chooser, const char *filename)
//{

//}

//void gtk_file_chooser_unselect_filename(GtkFileChooser *chooser, const char *filename)
//{

//}

//void gtk_file_chooser_select_all(GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_unselect_all (GtkFileChooser *chooser)
//{

//}

GSList *gtk_file_chooser_get_filenames(GtkFileChooser *chooser)
{
    GSList *files, *result;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    files = gtk_file_chooser_get_files (chooser);

    result = files_to_strings (files, g_file_get_path);
    g_slist_foreach (files, (GFunc) g_object_unref, NULL);
    g_slist_free (files);

    return result;
}

gboolean gtk_file_chooser_set_current_folder(GtkFileChooser *chooser, const gchar *filename)
{
    GFile *file;
    gboolean result;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), FALSE);
    g_return_val_if_fail (filename != NULL, FALSE);

    file = g_file_new_for_path (filename);
    result = gtk_file_chooser_set_current_folder_file (chooser, file, NULL);
    g_object_unref (file);

    return result;
}

//gchar *gtk_file_chooser_get_current_folder(GtkFileChooser *chooser)
//{

//}

/* URI manipulation
 */
gchar *gtk_file_chooser_get_uri(GtkFileChooser *chooser)
{
    GFile *file;
    gchar *result = NULL;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    file = gtk_file_chooser_get_file (chooser);
    if (file)
    {
        if (gtk_file_chooser_get_local_only (chooser))
        {
            gchar *local = g_file_get_path (file);
            if (local)
            {
                result = g_filename_to_uri (local, NULL, NULL);
                g_free (local);
            }
        }
        else
        {
            result = g_file_get_uri (file);
        }
        g_object_unref (file);
    }

    return result;
}

//gboolean gtk_file_chooser_set_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

//gboolean gtk_file_chooser_select_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

//void gtk_file_chooser_unselect_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

GSList * gtk_file_chooser_get_uris (GtkFileChooser *chooser)
{
    GSList *files, *result;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    files = gtk_file_chooser_get_files (chooser);

    if (gtk_file_chooser_get_local_only (chooser))
        result = files_to_strings (files, file_to_uri_with_native_path);
    else
        result = files_to_strings (files, g_file_get_uri);

    g_slist_foreach (files, (GFunc) g_object_unref, NULL);
    g_slist_free (files);

    return result;
}

gboolean gtk_file_chooser_set_current_folder_uri (GtkFileChooser *chooser, const gchar *uri)
{
    GFile *file;
    gboolean result;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), FALSE);
    g_return_val_if_fail (uri != NULL, FALSE);

    file = g_file_new_for_uri (uri);
    result = gtk_file_chooser_set_current_folder_file (chooser, file, NULL);
    g_object_unref (file);

    return result;
}

//gchar *gtk_file_chooser_get_current_folder_uri (GtkFileChooser *chooser)
//{

//}

/* GFile manipulation */
GFile *gtk_file_chooser_get_file (GtkFileChooser *chooser)
{
    GSList *list;
    GFile *result = NULL;

    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    list = gtk_file_chooser_get_files (chooser);
    if (list)
    {
        result = list->data;
        list = g_slist_delete_link (list, list);

        g_slist_foreach (list, (GFunc) g_object_unref, NULL);
        g_slist_free (list);
    }

    return result;
}

//gboolean gtk_file_chooser_set_file (GtkFileChooser *chooser, GFile *file, GError **error)
//{

//}

//gboolean gtk_file_chooser_select_file (GtkFileChooser *chooser, GFile *file, GError **error)
//{

//}

//void gtk_file_chooser_unselect_file (GtkFileChooser *chooser, GFile *file)
//{

//}

GSList *gtk_file_chooser_get_files (GtkFileChooser *chooser)
{
    GVariantIter *selected_files = NULL;
    gboolean ok = d_dbus_filedialog_call_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                              "selectedFiles",
                                                              NULL,
                                                              "(as)",
                                                              &selected_files);

    if (!ok) {
        g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

        return GTK_FILE_CHOOSER_GET_IFACE (chooser)->get_files (chooser);
    }

    if (!selected_files)
        return NULL;

    GSList *file_list = NULL;
    GSList *file_list_first = NULL;
    int selected_files_length = g_variant_iter_n_children(selected_files);

    for (int i = 0; i < selected_files_length; ++i) {
        const gchar *file_path = g_variant_get_string(g_variant_iter_next_value(selected_files), NULL);
        GFile *file = g_file_new_for_path(file_path);
        g_free(file_path);

        file_list = g_slist_append(file_list, file);

        if (!file_list_first) {
            file_list_first = file_list;
        }
    }

    g_variant_iter_free(selected_files);

    return file_list_first;
}

gboolean gtk_file_chooser_set_current_folder_file (GtkFileChooser *chooser, GFile *file, GError **error)
{
    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), FALSE);
    g_return_val_if_fail (G_IS_FILE (file), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    d_debug("uri: %s, path: %s\n", g_file_get_uri(file), g_file_get_path(file));

    d_dbus_filedialog_set_property_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                        "directoryUrl",
                                                        g_variant_new_string(g_file_get_uri(file)));

    return GTK_FILE_CHOOSER_GET_IFACE (chooser)->set_current_folder (chooser, file, error);
}

//GFile *gtk_file_chooser_get_current_folder_file (GtkFileChooser *chooser)
//{
//    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

//    return GTK_FILE_CHOOSER_GET_IFACE (chooser)->get_current_folder (chooser);
//}

/* Preview widget
 */
//void gtk_file_chooser_set_preview_widget (GtkFileChooser *chooser, GtkWidget *preview_widget)
//{

//}

//GtkWidget *gtk_file_chooser_get_preview_widget (GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_set_preview_widget_active (GtkFileChooser *chooser, gboolean active)
//{

//}

//gboolean gtk_file_chooser_get_preview_widget_active (GtkFileChooser *chooser)
//{

//}

//void gtk_file_chooser_set_use_preview_label (GtkFileChooser *chooser, gboolean use_label)
//{

//}

//gboolean gtk_file_chooser_get_use_preview_label (GtkFileChooser *chooser)
//{

//}

//char *gtk_file_chooser_get_preview_filename (GtkFileChooser *chooser)
//{

//}

//char *gtk_file_chooser_get_preview_uri (GtkFileChooser *chooser)
//{

//}

//GFile *gtk_file_chooser_get_preview_file (GtkFileChooser *chooser)
//{

//}

/* Extra widget
 */
//void gtk_file_chooser_set_extra_widget (GtkFileChooser *chooser, GtkWidget *extra_widget)
//{

//}

//GtkWidget *gtk_file_chooser_get_extra_widget (GtkFileChooser *chooser)
//{

//}

/* List of user selectable filters
 */

void gtk_file_chooser_add_filter (GtkFileChooser *chooser, GtkFileFilter *filter)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));

    GTK_FILE_CHOOSER_GET_IFACE (chooser)->add_filter (chooser, filter);

    d_update_filedialog_name_filters(chooser);
}

void gtk_file_chooser_remove_filter (GtkFileChooser *chooser, GtkFileFilter  *filter)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));

    GTK_FILE_CHOOSER_GET_IFACE (chooser)->remove_filter (chooser, filter);

    d_update_filedialog_name_filters(chooser);
}

//GSList *gtk_file_chooser_list_filters (GtkFileChooser *chooser)
//{

//}

/* Current filter
 */
void gtk_file_chooser_set_filter (GtkFileChooser *chooser, GtkFileFilter *filter)
{
    g_return_if_fail (GTK_IS_FILE_CHOOSER (chooser));
    g_return_if_fail (GTK_IS_FILE_FILTER (filter));

    g_object_set (chooser, "filter", filter, NULL);

    GByteArray *array = d_gtk_file_filter_to_string(filter);

    if (!array)
        return;

    if (array->len > 0) {
        g_byte_array_append(array, "\0", 1);
        d_dbus_filedialog_call_by_ghost_widget_sync(chooser,
                                                    "selectNameFilter",
                                                    g_variant_new("(s)", array->data),
                                                    NULL, NULL);
    }

    g_byte_array_free(array, FALSE);
}

//GtkFileFilter *gtk_file_chooser_get_filter (GtkFileChooser *chooser)
//{

//}

/* Per-application shortcut folders */
//gboolean gtk_file_chooser_add_shortcut_folder (GtkFileChooser *chooser, const char *folder, GError **error)
//{

//}

//gboolean gtk_file_chooser_remove_shortcut_folder (GtkFileChooser *chooser, const char *folder, GError **error)
//{

//}

//GSList *gtk_file_chooser_list_shortcut_folders (GtkFileChooser *chooser)
//{

//}

//gboolean gtk_file_chooser_add_shortcut_folder_uri (GtkFileChooser *chooser, const char *uri, GError **error)
//{

//}

//gboolean gtk_file_chooser_remove_shortcut_folder_uri (GtkFileChooser *chooser, const char *uri, GError **error)
//{

//}

//GSList *gtk_file_chooser_list_shortcut_folder_uris (GtkFileChooser *chooser)
//{

//}
