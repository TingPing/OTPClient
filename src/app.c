#include <gtk/gtk.h>
#include <gcrypt.h>
#include "otpclient.h"
#include "common.h"
#include "gquarks.h"
#include "imports.h"
#include "message-dialogs.h"
#include "password-cb.h"

#ifndef USE_FLATPAK_APP_FOLDER
static gchar     *get_db_path                        (GtkWidget *window);
#endif

static void       get_saved_window_size              (gint *width, gint *height);

static GtkWidget *create_main_window_with_header_bar (GtkApplication *app, gint width, gint height);

static void       get_window_size_cb                 (GtkWidget *window, GtkAllocation *allocation, gpointer user_data);

static void       add_data_cb                        (GtkWidget *btn, gpointer user_data);

static void       del_data_cb                        (GtkWidget *btn, gpointer user_data);

static void       import_cb                          (GtkWidget *btn, gpointer user_data);

static void       save_window_size                   (gint width, gint height);

static void       destroy_cb                         (GtkWidget *window, gpointer user_data);


void
activate (GtkApplication    *app,
          gpointer           user_data)
{
    gint64 memlock_limit = (gint64) user_data;
    gint32 max_file_size;
    if (memlock_limit == -1 || memlock_limit > 256000) {
        max_file_size = 256000; // memlock is either unlimited or bigger than what we need
    } else if (memlock_limit == -5) {
        max_file_size = 64000; // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    } else {
        max_file_size = (gint32) memlock_limit; // memlock is less than 256 KB
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    }

    gint widht, height;
    get_saved_window_size (&widht, &height);

    GtkWidget *main_window = create_main_window_with_header_bar (app, widht, height);
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
    g_signal_connect (main_window, "size-allocate", G_CALLBACK (get_window_size_cb), NULL);

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
        return;
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        show_message_dialog (main_window, "Couldn't initialize secure memory.\n", GTK_MESSAGE_ERROR);
        g_application_quit (G_APPLICATION (app));
        return;
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    DatabaseData *db_data = g_new0 (DatabaseData, 1);

#ifdef USE_FLATPAK_APP_FOLDER
    db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
#else
    db_data->db_path = get_db_path (main_window);
    if (db_data->db_path == NULL) {
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }
#endif

    db_data->max_file_size_from_memlock = max_file_size;
    db_data->objects_hash = NULL;
    db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));

    db_data->key = prompt_for_password (main_window, g_file_test (db_data->db_path, G_FILE_TEST_EXISTS));
    if (db_data->key == NULL) {
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (db_data->key);
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }

    GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
    GtkListStore *list_store = create_treeview (main_window, clipboard, db_data);
    g_object_set_data (G_OBJECT (main_window), "lstore", list_store);
    g_object_set_data (G_OBJECT (main_window), "clipboard", clipboard);

    g_signal_connect (find_widget (main_window, "add_btn_app"), "clicked", G_CALLBACK (add_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "del_btn_app"), "clicked", G_CALLBACK (del_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "imp_btn_app"), "clicked", G_CALLBACK (import_cb), db_data);
    g_signal_connect (main_window, "destroy", G_CALLBACK (destroy_cb), db_data);

    gtk_widget_show_all (main_window);
}


static void
get_saved_window_size (gint *width, gint *height)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    *width = 0;
    *height = 0;
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            *width = g_key_file_get_integer (kf, "config", "window_width", NULL);
            *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}


static GtkWidget *
create_main_window_with_header_bar (GtkApplication  *app, gint width, gint height)
{
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name (GTK_WINDOW (window), "otpclient");
    
    gtk_container_set_border_width (GTK_CONTAINER (window), 10);

    if (width > 0 && height > 0) {
        gtk_window_set_default_size (GTK_WINDOW (window), width, height);
    } else {
        gtk_window_set_default_size (GTK_WINDOW (window), 500, 350);
    }

    gchar *header_bar_text = g_malloc (strlen (APP_NAME ) + 1 + strlen (APP_VERSION) + 1);
    g_snprintf (header_bar_text, strlen (APP_NAME) + 1 + strlen (APP_VERSION) + 1, "%s %s", APP_NAME, APP_VERSION);
    header_bar_text[strlen(header_bar_text)] = '\0';

    GtkWidget *header_bar = create_header_bar (header_bar_text);
    GtkWidget *box = create_box_with_buttons ("add_btn_app", "del_btn_app", "imp_btn_app");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
    gtk_window_set_titlebar (GTK_WINDOW (window), header_bar);

    g_free (header_bar_text);

    return window;
}


static void
get_window_size_cb (GtkWidget      *window,
                    GtkAllocation  *allocation __attribute__((unused)),
                    gpointer        user_data  __attribute__((unused)))
{
    gint w, h;
    gtk_window_get_size (GTK_WINDOW (window), &w, &h);
    g_object_set_data (G_OBJECT (window), "width", GINT_TO_POINTER (w));
    g_object_set_data (G_OBJECT (window), "height", GINT_TO_POINTER (h));
}


#ifndef USE_FLATPAK_APP_FOLDER
static gchar *
get_db_path (GtkWidget *window)
{
    gchar *db_path = NULL;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", &err);
        if (db_path == NULL) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            return NULL;
        }
    } else {
        GtkWidget *dialog = gtk_file_chooser_dialog_new ("Select database location",
                                                         GTK_WINDOW (window),
                                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                                         "Cancel", GTK_RESPONSE_CANCEL,
                                                         "OK", GTK_RESPONSE_ACCEPT,
                                                         NULL);
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);
        gtk_file_chooser_set_select_multiple (chooser, FALSE);
        gtk_file_chooser_set_current_name (chooser, "NewDatabase.enc");
        gint res = gtk_dialog_run (GTK_DIALOG (dialog));
        if (res == GTK_RESPONSE_ACCEPT) {
            db_path = gtk_file_chooser_get_filename (chooser);
            g_key_file_set_string (kf, "config", "db_path", db_path);
            g_key_file_save_to_file (kf, cfg_file_path, &err);
            if (err != NULL) {
                g_printerr ("%s\n", err->message);
                g_key_file_free (kf);
            }
        }
        gtk_widget_destroy (dialog);
    }

    g_free (cfg_file_path);

    return db_path;
}
#endif


static void
add_data_cb (GtkWidget *btn,
             gpointer   user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    DatabaseData *db_data = (DatabaseData *)user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (top_level), "lstore");
    add_data_dialog (top_level, db_data, list_store);
}


static void
del_data_cb (GtkWidget *btn,
             gpointer   user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    DatabaseData *db_data = (DatabaseData *) user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (top_level), "lstore");
    const gchar *msg = "Do you really want to delete the selected entries?\n"
            "This will <b>permanently</b> delete these entries from the database";

    if (get_confirmation_from_dialog (top_level, msg)) {
        remove_selected_entries (db_data, list_store);
    }
}


static void
import_cb   (GtkWidget *btn,
             gpointer   user_data)
{
    DatabaseData *db_data = (DatabaseData *) user_data;

    GtkWidget *popover = gtk_popover_new (btn);
    gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);

    GtkWidget *button_box = gtk_button_box_new (GTK_ORIENTATION_VERTICAL);
    gtk_container_add (GTK_CONTAINER (popover), button_box);

    GtkWidget *bt1 = gtk_button_new_with_label ("andOTP");
    gtk_widget_set_name (bt1, ANDOTP_BTN_NAME);
    gtk_container_add(GTK_CONTAINER (button_box), bt1);
    g_signal_connect (bt1, "clicked", G_CALLBACK (select_file_cb), db_data);

    GtkWidget *bt2 = gtk_button_new_with_label ("Authenticator Plus");
    gtk_widget_set_name (bt2, AUTHPLUS_BTN_NAME);
    gtk_container_add(GTK_CONTAINER (button_box), bt2);
    g_signal_connect (bt2, "clicked", G_CALLBACK (select_file_cb), db_data);

    gtk_widget_show_all (popover);
}


static void
destroy_cb (GtkWidget   *window,
            gpointer     user_data)
{
    DatabaseData *db_data = (DatabaseData *) user_data;
    gcry_free (db_data->key);
    g_free (db_data->db_path);
    g_slist_free_full (db_data->objects_hash, g_free);
    json_node_free (db_data->json_data);
    g_free (db_data);
    gtk_clipboard_clear (g_object_get_data (G_OBJECT (window), "clipboard"));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
    gint w = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "width"));
    gint h = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "height"));
#pragma GCC diagnostic pop
    save_window_size (w, h);
}


static void
save_window_size (gint width,
                  gint height)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            g_key_file_set_integer (kf, "config", "window_width", width);
            g_key_file_set_integer (kf, "config", "window_height", height);
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                g_printerr ("%s\n", err->message);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}