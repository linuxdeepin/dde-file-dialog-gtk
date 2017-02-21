#include <gtk/gtk.h>
#include <gtk/gtkfilechooserprivate.h>
#include <gdk/x11/gdkx.h>

#include <X11/Xlib.h>

#define D_STRINGIFY(x) #x

#define DFM_FILEDIALOG_DBUS_SERVER "com.deepin.filemanager.filedialog"
#define DFM_FILEDIALOGMANAGER_DBUS_PATH "/com/deepin/filemanager/filedialogmanager"
#define DFM_FILEDIALOGMANAGER_DBUS_INTERFACE "com.deepin.filemanager.filedialogmanager"
#define DFM_FILEDIALOG_DBUS_INTERFACE "com.deepin.filemanager.filedialog"

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
        g_print("button text: %s, response id: %d\n", button_text, response_id);
        button_text = va_arg (varargs, const gchar *);
    }

    return result;
}

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
                                             const gchar        *method_name,
                                             GVariant           *parameters,
                                             const GVariantType *reply_type)
{
    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return NULL;
    }

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(dbus_connection,
                                                   DFM_FILEDIALOG_DBUS_SERVER,
                                                   object_path,
                                                   DFM_FILEDIALOG_DBUS_INTERFACE,
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
    }

    return result;
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

    GVariant *result = d_dbus_filedialog_call_sync(dbus_object_path,
                                                   method_name,
                                                   parameters,
                                                   (const GVariantType *)reply_type);

    if (reply_data)
        g_variant_get(result, reply_type, reply_data);

    if (result) {
        const gchar *tmp_variant_print = g_variant_print(result, TRUE);
        const gchar *tmp_variant_type_string = g_variant_get_type_string(result);

        g_print("d_dbus_filedialog_call_by_ghost_widget_sync: %s, type=%s\n", tmp_variant_print, tmp_variant_type_string);
        g_free(tmp_variant_print);
        g_variant_unref(result);
    }

    return TRUE;
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

        g_print("d_dbus_filedialog_get_property_by_ghost_widget_sync: %s, type=%s\n", tmp_variant_print, tmp_variant_type_string);
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
        g_warning("Get property \"%s\" is failed, %s", property_name, error->message);
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

    if (gtk_file_chooser_get_action(widget_ghost) == GTK_FILE_CHOOSER_ACTION_SAVE) {
        GFile *file = gtk_file_chooser_get_file(widget_ghost);

        if (file) {
            gtk_file_chooser_set_current_name(widget_ghost, g_file_get_basename(file));
            g_object_unref(file);
        }
    }

    g_variant_get(parameters, "(i)", &dialog_code);
    gtk_dialog_response(GTK_DIALOG(widget_ghost), dialog_code == Rejected ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_ACCEPT);
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

    for (int i = 0; i < g_variant_iter_n_children(selected_files); ++i) {
        const gchar *file_uri = g_variant_get_string(g_variant_iter_next_value(selected_files), NULL);

        gtk_file_chooser_select_uri(GTK_FILE_CHOOSER_DIALOG(widget_ghost), file_uri);
        g_print("selected file uri=%s\n", file_uri);
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

    g_print("dbus file dialog current uri: %s\n", current_url);

    gtk_file_chooser_set_current_folder_uri(widget_ghost, current_url);
    g_free(current_url);
}

static void d_on_gtk_filedialog_destroy(GtkWidget *object,
                                        gpointer   user_data)
{
    (void)object;
    (void)user_data;
}

GtkWidget *gtk_file_chooser_dialog_new(const gchar          *title,
                                       GtkWindow            *parent,
                                       GtkFileChooserAction  action,
                                       const gchar          *first_button_text,
                                       ...)
{
    va_list varargs;
    va_start (varargs, first_button_text);
    GtkWidget *result = gtk_file_chooser_dialog_new_valist (title, parent, action,
                                                 NULL, first_button_text,
                                                 varargs);
    va_end (varargs);

    GDBusConnection *dbus_connection = get_connection();

    if (!dbus_connection) {
        g_warning("Get dbus connection failed\n");

        return result;
    }

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(dbus_connection,
                                                  DFM_FILEDIALOG_DBUS_SERVER,
                                                  DFM_FILEDIALOGMANAGER_DBUS_PATH,
                                                  DFM_FILEDIALOGMANAGER_DBUS_INTERFACE,
                                                  "createDialog",
                                                  g_variant_new("(s)", ""),
                                                  ((const GVariantType *) "(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  5 * 1000,
                                                  NULL,
                                                  &error);


    if (error) {
        g_warning("Call \"createDialog\" is failed, %s", error->message);
        g_error_free(error);

        return result;
    }

    gtk_window_set_decorated(GTK_WINDOW(result), FALSE);
//    gtk_window_set_skip_pager_hint(GTK_WINDOW(result), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(result), FALSE);
    gtk_window_set_opacity(GTK_WINDOW(result), 0);
    gtk_widget_set_sensitive(result, FALSE);

    gchar *dbus_object_path = NULL;

    g_variant_get(reply, "(o)", &dbus_object_path);
    g_variant_unref(reply);

    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW (result), parent);
    }

    g_object_set_data(GTK_OBJECT(result), D_STRINGIFY(_d_dbus_file_dialog_object_path), dbus_object_path);

    // GTK widget mapping to DBus file dialog
    g_signal_connect(result, "show", G_CALLBACK(d_show_dbus_filedialog), result);
    g_signal_connect(result, "hide", G_CALLBACK(d_hide_dbus_filedialog), result);

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

    gtk_file_chooser_set_action(GTK_FILE_CHOOSER(result), action);

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
                                                    g_variant_new_int32(Directory),
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

    d_dbus_filedialog_call_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                "setFileMode",
                                                g_variant_new_int32(select_multiple ? ExistingFiles : ExistingFile),
                                                NULL, NULL);
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

//GSList *gtk_file_chooser_get_filenames(GtkFileChooser *chooser)
//{

//}

//gboolean gtk_file_chooser_set_current_folder(GtkFileChooser *chooser, const gchar *filename)
//{

//}

//gchar *gtk_file_chooser_get_current_folder(GtkFileChooser *chooser)
//{

//}

/* URI manipulation
 */
//gchar *gtk_file_chooser_get_uri(GtkFileChooser *chooser)
//{

//}

//gboolean gtk_file_chooser_set_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

//gboolean gtk_file_chooser_select_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

//void gtk_file_chooser_unselect_uri (GtkFileChooser *chooser, const char *uri)
//{

//}

//GSList * gtk_file_chooser_get_uris (GtkFileChooser *chooser)
//{

//}

//gboolean gtk_file_chooser_set_current_folder_uri (GtkFileChooser *chooser, const gchar *uri)
//{

//}

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

    for (int i = 0; i < g_variant_iter_n_children(selected_files); ++i) {
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

    g_print("gtk_file_chooser_set_current_folder_file: %s\n", g_file_get_uri(file));

    d_dbus_filedialog_call_by_ghost_widget_sync(GTK_WIDGET(chooser),
                                                "setDirectoryUrl",
                                                g_variant_new_string(g_file_get_uri(file)),
                                                NULL, NULL);

    return GTK_FILE_CHOOSER_GET_IFACE (chooser)->set_current_folder (chooser, file, error);
}

GFile *gtk_file_chooser_get_current_folder_file (GtkFileChooser *chooser)
{
    g_return_val_if_fail (GTK_IS_FILE_CHOOSER (chooser), NULL);

    g_print("gtk_file_chooser_get_current_folder_file\n");

    return GTK_FILE_CHOOSER_GET_IFACE (chooser)->get_current_folder (chooser);
}

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
//void gtk_file_chooser_add_filter (GtkFileChooser *chooser, GtkFileFilter *filter)
//{

//}

//void gtk_file_chooser_remove_filter (GtkFileChooser *chooser, GtkFileFilter  *filter)
//{

//}

//GSList *gtk_file_chooser_list_filters (GtkFileChooser *chooser)
//{

//}

/* Current filter
 */
//void gtk_file_chooser_set_filter (GtkFileChooser *chooser, GtkFileFilter *filter)
//{

//}

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
