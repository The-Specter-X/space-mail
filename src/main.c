#include "mail.h"

#include <gmime/gmime.h>
#include <glib/gstdio.h>
#include <libxapp/xapp-status-icon.h>
#include <libsecret/secret.h>
#include <string.h>

enum { COL_ID, COL_FROM, COL_SUBJECT, COL_FOLDER, COL_DATE, COL_ACCOUNT, COL_COUNT };

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *tree;
    GtkWidget *preview;
    GtkWidget *status;
    GtkWidget *search;
    GtkWidget *account_picker;
    GtkWidget *offline_button;
    GtkListStore *list;
    XAppStatusIcon *tray;
    GPtrArray *accounts;
    MailStore store;
    gboolean offline;
    gboolean background;
    gboolean notifications;
    gboolean show_tray;
    gboolean autostart;
    gboolean held;
    gboolean start_hidden;
    gboolean busy;
    gboolean quit_after_sync;
    guint timer;
    GCancellable *sync_cancel;
} MailApp;

typedef struct {
    MailApp *app;
    guint new_messages;
} SyncJob;

static void quit_app(GtkWidget *widget, gpointer user);

static void show_error(MailApp *app, const char *text)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE, "%s", text);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void status(MailApp *app, const char *message)
{
    gtk_label_set_text(GTK_LABEL(app->status), message);
}

static MailAccount *current_account(MailApp *app)
{
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(app->account_picker));
    return index >= 0 && (guint)index < app->accounts->len ?
        g_ptr_array_index(app->accounts, index) : NULL;
}

static void reload_picker(MailApp *app)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->account_picker));
    for (guint i = 0; i < app->accounts->len; ++i) {
        MailAccount *a = g_ptr_array_index(app->accounts, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->account_picker), a->address);
    }
    if (app->accounts->len) gtk_combo_box_set_active(GTK_COMBO_BOX(app->account_picker), 0);
}

static void reload_list(MailApp *app)
{
    gtk_list_store_clear(app->list);
    const char *query = gtk_entry_get_text(GTK_ENTRY(app->search));
    GPtrArray *rows = mail_store_list(&app->store, query);
    for (guint i = 0; i < rows->len; ++i) {
        MailRow *row = g_ptr_array_index(rows, i);
        GtkTreeIter iter;
        char *folder = row->on_server ? g_strdup(row->folder) :
            g_strdup_printf("%s · local copy", row->folder);
        char *sender = g_utf8_make_valid(row->sender, -1);
        char *subject = g_utf8_make_valid(row->subject, -1);
        char *safe_folder = g_utf8_make_valid(folder, -1);
        char *date = g_utf8_make_valid(row->date, -1);
        gtk_list_store_append(app->list, &iter);
        gtk_list_store_set(app->list, &iter,
            COL_ID, row->id, COL_FROM, sender, COL_SUBJECT, subject,
            COL_FOLDER, safe_folder, COL_DATE, date, COL_ACCOUNT, row->account, -1);
        g_free(folder);
        g_free(sender);
        g_free(subject);
        g_free(safe_folder);
        g_free(date);
    }
    g_ptr_array_unref(rows);
}

static gboolean selected_id(MailApp *app, gint64 *id)
{
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree)), &model, &iter))
        return FALSE;
    gtk_tree_model_get(model, &iter, COL_ID, id, -1);
    return TRUE;
}

static void selected_changed(GtkTreeSelection *selection, gpointer user)
{
    MailApp *app = user;
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTextBuffer *text = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->preview));
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_text_buffer_set_text(text, "", -1);
        return;
    }
    gint64 id;
    gtk_tree_model_get(model, &iter, COL_ID, &id, -1);
    GBytes *bytes = mail_store_read(&app->store, id);
    if (!bytes) return;
    gsize size;
    const guint8 *raw = g_bytes_get_data(bytes, &size);
    char *body = mail_message_preview(raw, size);
    gtk_text_buffer_set_text(text, *body ? body :
        "This message has no readable text body. You can export its original .eml file.", -1);
    g_free(body);
    g_bytes_unref(bytes);
}

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), SPACE_APP_ID, "settings.ini", NULL);
}

static void load_settings(MailApp *app)
{
    char *path = settings_path();
    GKeyFile *file = g_key_file_new();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        app->offline = g_key_file_get_boolean(file, "Privacy", "offline", NULL);
        app->background = g_key_file_get_boolean(file, "Desktop", "background", NULL);
        app->notifications = g_key_file_get_boolean(file, "Desktop", "notifications", NULL);
        app->show_tray = g_key_file_get_boolean(file, "Desktop", "tray", NULL);
        app->autostart = g_key_file_get_boolean(file, "Desktop", "autostart", NULL);
    }
    g_key_file_free(file);
    g_free(path);
}

static gboolean save_settings(MailApp *app)
{
    char *path = settings_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GKeyFile *file = g_key_file_new();
    g_key_file_set_boolean(file, "Privacy", "offline", app->offline);
    g_key_file_set_boolean(file, "Desktop", "background", app->background);
    g_key_file_set_boolean(file, "Desktop", "notifications", app->notifications);
    g_key_file_set_boolean(file, "Desktop", "tray", app->show_tray);
    g_key_file_set_boolean(file, "Desktop", "autostart", app->autostart);
    gsize len;
    char *data = g_key_file_to_data(file, &len, NULL);
    gboolean ok = g_file_set_contents(path, data, (gssize)len, NULL);
    g_free(data);
    g_key_file_free(file);
    g_free(dir);
    g_free(path);
    return ok;
}

static gboolean update_autostart(MailApp *app)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    char *path = g_build_filename(dir, SPACE_APP_ID ".desktop", NULL);
    gboolean ok = TRUE;
    if (app->autostart) {
        const char *entry = app->background
            ? "[Desktop Entry]\nType=Application\nName=Space Mail\nExec=space-mail --background\nX-GNOME-Autostart-enabled=true\n"
            : "[Desktop Entry]\nType=Application\nName=Space Mail\nExec=space-mail\nX-GNOME-Autostart-enabled=true\n";
        ok = g_mkdir_with_parents(dir, 0700) == 0 &&
            g_file_set_contents(path, entry, -1, NULL);
    } else {
        ok = g_unlink(path) == 0 || !g_file_test(path, G_FILE_TEST_EXISTS);
    }
    g_free(path);
    g_free(dir);
    return ok;
}

static void set_tray(MailApp *app)
{
    if (app->show_tray && !app->tray) {
        app->tray = xapp_status_icon_new_with_name(SPACE_APP_ID);
        xapp_status_icon_set_icon_name(app->tray, "mail-unread-symbolic");
        xapp_status_icon_set_tooltip_text(app->tray, "Space Mail");
        GtkWidget *menu = gtk_menu_new();
        GtkWidget *open = gtk_menu_item_new_with_label("Open Space Mail");
        GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
        g_signal_connect_swapped(open, "activate", G_CALLBACK(gtk_window_present), app->window);
        g_signal_connect(quit, "activate", G_CALLBACK(quit_app), app);
        gtk_widget_show_all(menu);
        xapp_status_icon_set_primary_menu(app->tray, GTK_MENU(menu));
    }
    if (app->tray) xapp_status_icon_set_visible(app->tray, app->show_tray);
}

static gboolean close_window(GtkWidget *widget, GdkEvent *event, gpointer user)
{
    (void)event;
    MailApp *app = user;
    if (app->background) {
        gtk_widget_hide(widget);
        return TRUE;
    }
    if (app->busy) {
        app->quit_after_sync = TRUE;
        gtk_widget_hide(widget);
        return TRUE;
    }
    return FALSE;
}

static void quit_app(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    if (app->busy) {
        app->quit_after_sync = TRUE;
        gtk_widget_hide(app->window);
    } else g_application_quit(G_APPLICATION(app->application));
}

static void sync_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    SyncJob *job = data;
    MailApp *app = job->app;
    GError *error = NULL;
    for (guint i = 0; i < app->accounts->len; ++i) {
        if (g_cancellable_set_error_if_cancelled(cancel, &error)) break;
        MailAccount *account = g_ptr_array_index(app->accounts, i);
        char *password = mail_password_lookup(account, &error);
        if (!password) {
            if (!error)
                g_set_error(&error, SPACE_ERR, SPACE_ERROR_AUTH,
                            "No password stored for %s. Add it in Account settings.", account->address);
            break;
        }
        gboolean ok = mail_network_sync(&app->store, account, password,
                                        &job->new_messages, cancel, &error);
        if (ok) ok = mail_network_send_outbox(&app->store, account, password, cancel, &error);
        secret_password_free(password);
        if (!ok) break;
    }
    if (error) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
}

static void sync_complete(GObject *source, GAsyncResult *result, gpointer user)
{
    (void)source;
    SyncJob *job = user;
    MailApp *app = job->app;
    GError *error = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
    app->busy = FALSE;
    g_clear_object(&app->sync_cancel);
    reload_list(app);
    if (!ok) {
        status(app, app->offline && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            ? "Offline mode · local mail only" : error->message);
        g_clear_error(&error);
    } else {
        char *message = g_strdup_printf("Synced · %u new local messages", job->new_messages);
        status(app, message);
        if (job->new_messages && app->notifications) {
            GNotification *notification = g_notification_new("New mail");
            g_notification_set_body(notification, message);
            g_application_send_notification(G_APPLICATION(app->application), NULL, notification);
            g_object_unref(notification);
        }
        g_free(message);
    }
    g_application_release(G_APPLICATION(app->application));
    if (app->quit_after_sync) g_application_quit(G_APPLICATION(app->application));
    g_free(job);
}

static void start_sync(MailApp *app)
{
    if (app->offline) { status(app, "Offline mode · local mail only"); return; }
    if (app->busy) return;
    if (!app->accounts->len) { status(app, "Add an account to start syncing"); return; }
    app->busy = TRUE;
    app->sync_cancel = g_cancellable_new();
    g_application_hold(G_APPLICATION(app->application));
    status(app, "Syncing securely…");
    SyncJob *job = g_new0(SyncJob, 1);
    job->app = app;
    GTask *task = g_task_new(NULL, app->sync_cancel, sync_complete, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, sync_worker);
    g_object_unref(task);
}

static gboolean sync_timer(gpointer data)
{
    MailApp *app = data;
    if (!app->offline && (gtk_widget_get_visible(app->window) || app->background))
        start_sync(app);
    return G_SOURCE_CONTINUE;
}

static void on_sync(GtkWidget *widget, gpointer user)
{
    (void)widget;
    start_sync(user);
}

static void retry_uncertain(GtkWidget *button, gpointer user)
{
    MailApp *app = user;
    gint64 *id = g_object_get_data(G_OBJECT(button), "outbox-id");
    GtkWidget *prompt = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "Retry this message?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(prompt),
        "Its previous delivery could not be confirmed. Retrying may send a duplicate.");
    gtk_dialog_add_buttons(GTK_DIALOG(prompt), "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Retry", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(prompt)) == GTK_RESPONSE_ACCEPT) {
        GError *error = NULL;
        if (!mail_store_retry(&app->store, *id, &error))
            show_error(app, error ? error->message : "Could not queue retry");
        else status(app, "Message queued again · it may duplicate a delivered message");
        g_clear_error(&error);
    }
    gtk_widget_destroy(prompt);
}

static void on_offline(GtkToggleButton *button, gpointer user)
{
    MailApp *app = user;
    app->offline = gtk_toggle_button_get_active(button);
    if (app->offline && app->sync_cancel) g_cancellable_cancel(app->sync_cancel);
    save_settings(app);
    status(app, app->offline ? "Offline mode · no network requests" :
                             "Online · syncs while open");
    if (!app->offline) start_sync(app);
}

static GtkWidget *entry_row(GtkWidget *grid, int row, const char *label,
                            const char *value)
{
    GtkWidget *name = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), value ? value : "");
    gtk_grid_attach(GTK_GRID(grid), name, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
}

static gboolean host_valid(const char *host)
{
    if (!host || !*host) return FALSE;
    for (const char *p = host; *p; ++p)
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-') return FALSE;
    return TRUE;
}

static void add_account(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    if (app->busy) { status(app, "Wait for sync to finish before adding an account"); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add mail account", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    GtkWidget *address = entry_row(grid, 0, "Email address", "");
    GtkWidget *username = entry_row(grid, 1, "Login name", "");
    GtkWidget *password = entry_row(grid, 2, "Password / app password", "");
    gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
    GtkWidget *imap = entry_row(grid, 3, "IMAP server", "");
    GtkWidget *imap_port = entry_row(grid, 4, "IMAP TLS port", "993");
    GtkWidget *smtp = entry_row(grid, 5, "SMTP server", "");
    GtkWidget *smtp_port = entry_row(grid, 6, "SMTP port", "587");
    GtkWidget *starttls = gtk_check_button_new_with_label("Use STARTTLS for SMTP (port 587)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(starttls), TRUE);
    gtk_grid_attach(GTK_GRID(grid), starttls, 0, 7, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       grid, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *addr = gtk_entry_get_text(GTK_ENTRY(address));
        const char *user_name = gtk_entry_get_text(GTK_ENTRY(username));
        const char *pass = gtk_entry_get_text(GTK_ENTRY(password));
        const char *ihost = gtk_entry_get_text(GTK_ENTRY(imap));
        const char *shost = gtk_entry_get_text(GTK_ENTRY(smtp));
        char *end_i = NULL, *end_s = NULL;
        guint64 ip = g_ascii_strtoull(gtk_entry_get_text(GTK_ENTRY(imap_port)), &end_i, 10);
        guint64 sp = g_ascii_strtoull(gtk_entry_get_text(GTK_ENTRY(smtp_port)), &end_s, 10);
        if (!strchr(addr, '@') || !*user_name || !host_valid(ihost) ||
            !host_valid(shost) || ip == 0 || ip > 65535 || sp == 0 || sp > 65535 ||
            !end_i || *end_i || !end_s || *end_s || !*pass) {
            show_error(app, "Enter an email address, login, password, valid hostnames and ports.");
            continue;
        }
        MailAccount *account = g_new0(MailAccount, 1);
        account->id = g_uuid_string_random();
        account->address = g_strdup(addr);
        account->username = g_strdup(user_name);
        account->imap_host = g_strdup(ihost);
        account->imap_port = (guint)ip;
        account->smtp_host = g_strdup(shost);
        account->smtp_port = (guint)sp;
        account->smtp_starttls = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(starttls));
        GError *error = NULL;
        if (!mail_password_save(account, pass, &error) ||
            !mail_account_save(account, &error)) {
            show_error(app, error ? error->message : "Could not save account");
            g_clear_error(&error);
            mail_account_free(account);
            continue;
        }
        g_ptr_array_add(app->accounts, account);
        reload_picker(app);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->account_picker), app->accounts->len - 1);
        status(app, "Account added · password stored in your desktop Secret Service");
        break;
    }
    gtk_widget_destroy(dialog);
}

static void compose(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    MailAccount *account = current_account(app);
    if (!account) { show_error(app, "Add an account first."); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("New message", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Attach file", 3,
        "_Save draft", 1, "_Queue message", 2, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 650, 480);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *to = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(to), "To: one email address");
    GtkWidget *subject = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(subject), "Subject");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *body = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(body), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    gtk_box_pack_start(GTK_BOX(box), to, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), subject, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       box, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    int response;
    char *attachment_path = NULL;
    while ((response = gtk_dialog_run(GTK_DIALOG(dialog))) > 0) {
        if (response == 3) {
            GtkWidget *choose = gtk_file_chooser_dialog_new("Attach file", GTK_WINDOW(dialog),
                GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
                "_Attach", GTK_RESPONSE_ACCEPT, NULL);
            if (gtk_dialog_run(GTK_DIALOG(choose)) == GTK_RESPONSE_ACCEPT) {
                g_free(attachment_path);
                attachment_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(choose));
                char *name = g_path_get_basename(attachment_path);
                char *label = g_strdup_printf("Attached: %s", name);
                gtk_button_set_label(GTK_BUTTON(gtk_dialog_get_widget_for_response(
                    GTK_DIALOG(dialog), 3)), label);
                g_free(label);
                g_free(name);
            }
            gtk_widget_destroy(choose);
            continue;
        }
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(body));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        const char *recipient = gtk_entry_get_text(GTK_ENTRY(to));
        GError *error = NULL;
        GBytes *raw = mail_compose_raw(account, recipient,
            gtk_entry_get_text(GTK_ENTRY(subject)), text, attachment_path, &error);
        if (raw) {
            gsize size;
            const guint8 *data = g_bytes_get_data(raw, &size);
            gboolean ok = response == 1
                ? mail_store_message(&app->store, account->id, "Drafts (local)",
                                     0, 0, data, size, &error)
                : mail_store_queue(&app->store, account->id, recipient,
                                   data, size, &error);
            g_bytes_unref(raw);
            if (ok) {
                status(app, response == 1 ? "Draft saved locally" :
                    "Message queued locally. It will send at the next successful sync.");
                reload_list(app);
                if (response == 2 && !app->offline) start_sync(app);
                g_free(text);
                break;
            }
        }
        show_error(app, error ? error->message : "Could not save message");
        g_clear_error(&error);
        g_free(text);
    }
    gtk_widget_destroy(dialog);
    g_free(attachment_path);
}

static void save_attachments(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    gint64 id;
    if (!selected_id(app, &id)) { show_error(app, "Select a message first."); return; }
    GBytes *raw = mail_store_read(&app->store, id);
    if (!raw) return;
    gsize length;
    const guint8 *data = g_bytes_get_data(raw, &length);
    GPtrArray *attachments = mail_message_attachments(data, length);
    g_bytes_unref(raw);
    if (!attachments->len) {
        status(app, "No attachments in this message");
        g_ptr_array_unref(attachments);
        return;
    }
    for (guint i = 0; i < attachments->len; ++i) {
        MailAttachment *attachment = g_ptr_array_index(attachments, i);
        GtkWidget *dialog = gtk_file_chooser_dialog_new("Save attachment",
            GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Skip", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), attachment->filename);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            gsize size;
            const char *contents = g_bytes_get_data(attachment->data, &size);
            GError *error = NULL;
            if (!g_file_set_contents(path, contents, (gssize)size, &error))
                show_error(app, error->message);
            else status(app, "Attachment saved");
            g_clear_error(&error);
            g_free(path);
        }
        gtk_widget_destroy(dialog);
    }
    g_ptr_array_unref(attachments);
}

static void import_message(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Import .eml message",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Import", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        if (!mail_store_import_eml(&app->store, path, &error))
            show_error(app, error->message);
        else { status(app, "Message imported into Local Archive"); reload_list(app); }
        g_clear_error(&error);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void import_mbox(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Import mbox archive",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Import", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        guint imported = mail_store_import_mbox(&app->store, path, &error);
        if (error) show_error(app, error->message);
        else {
            char *message = g_strdup_printf("Imported %u messages into Local Archive", imported);
            status(app, message);
            g_free(message);
            reload_list(app);
        }
        g_clear_error(&error);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void export_mbox(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Export all mail to mbox",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "space-mail.mbox");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        if (!mail_store_export_mbox(&app->store, path, &error))
            show_error(app, error ? error->message : "Could not export archive");
        else status(app, "All messages exported to mbox");
        g_clear_error(&error);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void export_message(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    gint64 id;
    if (!selected_id(app, &id)) { show_error(app, "Select a message first."); return; }
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Export original .eml",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "message.eml");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        if (!mail_store_export_eml(&app->store, id, path, &error))
            show_error(app, error->message);
        else status(app, "Original message exported");
        g_clear_error(&error);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void clean_missing(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    if (app->busy) { status(app, "Wait for sync to finish before cleaning"); return; }
    guint count = mail_store_missing_count(&app->store);
    if (!count) { status(app, "No server-deleted local copies to clean"); return; }
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "Delete %u local messages missing from their servers?", count);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "This removes their local copies and search entries. Local archives, drafts and queued messages are kept. This cannot be undone.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Delete local copies", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GError *error = NULL;
        if (!mail_store_clean_missing(&app->store, &error))
            show_error(app, error->message);
        else { reload_list(app); status(app, "Local copies cleaned"); }
        g_clear_error(&error);
    }
    gtk_widget_destroy(dialog);
}

static void show_outbox(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    MailAccount *account = current_account(app);
    if (!account) { show_error(app, "Select an account first."); return; }
    GPtrArray *rows = mail_store_outbox(&app->store, account->id);
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Outbox", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    if (!rows->len) gtk_box_pack_start(GTK_BOX(box), gtk_label_new("No queued messages"), FALSE, FALSE, 5);
    for (guint i = 0; i < rows->len; ++i) {
        MailRow *row = g_ptr_array_index(rows, i);
        char *label = g_strdup_printf("%s · %s", row->sender,
            row->on_server ? "queued" : "delivery uncertain; manual retry only");
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(line), gtk_label_new(label), TRUE, TRUE, 0);
        if (!row->on_server) {
            GtkWidget *retry = gtk_button_new_with_label("Retry (may duplicate)");
            g_object_set_data_full(G_OBJECT(retry), "outbox-id",
                g_memdup2(&row->id, sizeof row->id), g_free);
            g_signal_connect(retry, "clicked", G_CALLBACK(retry_uncertain), app);
            gtk_box_pack_start(GTK_BOX(line), retry, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(box), line, FALSE, FALSE, 5);
        g_free(label);
    }
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_ptr_array_unref(rows);
}

static void show_settings(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Preferences", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    GtkWidget *background = gtk_check_button_new_with_label("Keep running after closing the window");
    GtkWidget *notifications = gtk_check_button_new_with_label("Notify me about new mail");
    GtkWidget *tray = gtk_check_button_new_with_label("Show a tray icon");
    GtkWidget *autostart = gtk_check_button_new_with_label("Start at login");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(background), app->background);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications), app->notifications);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray), app->show_tray);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(autostart), app->autostart);
    gtk_box_pack_start(GTK_BOX(box), background, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), notifications, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tray, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), autostart, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_label_new("Offline mode is available in the main window."),
        FALSE, FALSE, 6);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gboolean next_background = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(background));
    if (next_background && !app->held) {
        g_application_hold(G_APPLICATION(app->application));
        app->held = TRUE;
    } else if (!next_background && app->held) {
        g_application_release(G_APPLICATION(app->application));
        app->held = FALSE;
    }
    app->background = next_background;
    app->notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications));
    app->show_tray = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tray));
    app->autostart = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autostart));
    save_settings(app);
    if (!update_autostart(app)) show_error(app, "Could not update login startup preference");
    set_tray(app);
    gtk_widget_destroy(dialog);
}

static void change_password(GtkWidget *widget, gpointer user)
{
    (void)widget;
    MailApp *app = user;
    MailAccount *account = current_account(app);
    if (!account) { show_error(app, "Add an account first."); return; }
    if (app->busy) { status(app, "Wait for sync to finish before changing a password"); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Update account password",
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(account->address), FALSE, FALSE, 0);
    GtkWidget *password = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(password), "New password or app password");
    gtk_box_pack_start(GTK_BOX(box), password, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *value = gtk_entry_get_text(GTK_ENTRY(password));
        GError *error = NULL;
        if (!*value) show_error(app, "Enter a password.");
        else if (!mail_password_save(account, value, &error))
            show_error(app, error ? error->message : "Could not update password");
        else status(app, "Password updated in your desktop Secret Service");
        g_clear_error(&error);
    }
    gtk_widget_destroy(dialog);
}

static GtkWidget *toolbar_button(GtkWidget *bar, const char *label,
                                 GCallback callback, MailApp *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_box_pack_start(GTK_BOX(bar), button, FALSE, FALSE, 0);
    g_signal_connect(button, "clicked", callback, app);
    return button;
}

static void action_item(GtkWidget *menu, const char *label,
                        GCallback callback, MailApp *app)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(item, "activate", callback, app);
}

static void create_window(MailApp *app)
{
    app->window = gtk_application_window_new(app->application);
    gtk_window_set_title(GTK_WINDOW(app->window), "Space Mail");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1050, 720);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(close_window), app);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Space Mail");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Mail stays on this computer");
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 10);
    gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);
    app->account_picker = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->account_picker, 170, -1);
    gtk_box_pack_start(GTK_BOX(bar), app->account_picker, FALSE, FALSE, 0);
    toolbar_button(bar, "Add account", G_CALLBACK(add_account), app);
    toolbar_button(bar, "New", G_CALLBACK(compose), app);
    toolbar_button(bar, "Sync", G_CALLBACK(on_sync), app);
    app->offline_button = gtk_check_button_new_with_label("Offline");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->offline_button), app->offline);
    gtk_box_pack_start(GTK_BOX(bar), app->offline_button, FALSE, FALSE, 0);
    g_signal_connect(app->offline_button, "toggled", G_CALLBACK(on_offline), app);
    app->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->search), "Search local mail");
    gtk_box_pack_end(GTK_BOX(bar), app->search, TRUE, TRUE, 0);
    g_signal_connect_swapped(app->search, "search-changed", G_CALLBACK(reload_list), app);

    GtkWidget *actions = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(actions), "More");
    GtkWidget *menu = gtk_menu_new();
    action_item(menu, "Outbox", G_CALLBACK(show_outbox), app);
    action_item(menu, "Import .eml", G_CALLBACK(import_message), app);
    action_item(menu, "Import mbox", G_CALLBACK(import_mbox), app);
    action_item(menu, "Export selected .eml", G_CALLBACK(export_message), app);
    action_item(menu, "Export all as mbox", G_CALLBACK(export_mbox), app);
    action_item(menu, "Save attachments", G_CALLBACK(save_attachments), app);
    action_item(menu, "Clean missing local copies", G_CALLBACK(clean_missing), app);
    action_item(menu, "Update account password", G_CALLBACK(change_password), app);
    action_item(menu, "Preferences", G_CALLBACK(show_settings), app);
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(actions), menu);
    gtk_box_pack_start(GTK_BOX(bar), actions, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);
    app->list = gtk_list_store_new(COL_COUNT, G_TYPE_INT64, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->list));
    const char *titles[] = { "From", "Subject", "Folder", "Date", "Account" };
    const int columns[] = { COL_FROM, COL_SUBJECT, COL_FOLDER, COL_DATE, COL_ACCOUNT };
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            titles[i], cell, "text", columns[i], NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree), column);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree)),
                     "changed", G_CALLBACK(selected_changed), app);
    GtkWidget *mail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(mail_scroll), app->tree);
    gtk_paned_pack1(GTK_PANED(paned), mail_scroll, TRUE, FALSE);
    app->preview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->preview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->preview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->preview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->preview), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->preview), 12);
    GtkWidget *preview_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(preview_scroll), app->preview);
    gtk_paned_pack2(GTK_PANED(paned), preview_scroll, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 380);
    app->status = gtk_label_new(app->offline ? "Offline · local mail only" :
                                             "Ready · mail is stored locally");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0);
    gtk_widget_set_margin_start(app->status, 12);
    gtk_widget_set_margin_bottom(app->status, 8);
    gtk_box_pack_end(GTK_BOX(root), app->status, FALSE, FALSE, 0);
    reload_picker(app);
    reload_list(app);
    gtk_widget_show_all(app->window);
    set_tray(app);
    if (app->background && !app->held) {
        g_application_hold(G_APPLICATION(app->application));
        app->held = TRUE;
    }
    if (!app->offline && app->accounts->len) start_sync(app);
}

static void activate(GtkApplication *application, gpointer user)
{
    (void)application;
    MailApp *app = user;
    if (!app->window) {
        create_window(app);
        if (app->start_hidden && app->background) {
            gtk_widget_hide(app->window);
            app->start_hidden = FALSE;
            return;
        }
    }
    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv)
{
    MailApp app = { 0 };
    gboolean start_hidden = argc > 1 && g_strcmp0(argv[1], "--background") == 0;
    if (start_hidden) argc = 1;
    GError *error = NULL;
    g_mime_init();
    app.accounts = mail_accounts_load(&error);
    if (!app.accounts || !mail_store_open(&app.store, &error)) {
        g_printerr("Space Mail: %s\n", error ? error->message : "Cannot open data");
        g_clear_error(&error);
        if (app.accounts) g_ptr_array_unref(app.accounts);
        mail_store_close(&app.store);
        g_mime_shutdown();
        return 1;
    }
    load_settings(&app);
    app.start_hidden = start_hidden;
    app.application = gtk_application_new(SPACE_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    app.timer = g_timeout_add_seconds(300, sync_timer, &app);
    int result = g_application_run(G_APPLICATION(app.application), argc, argv);
    g_source_remove(app.timer);
    if (app.tray) g_object_unref(app.tray);
    if (app.list) g_object_unref(app.list);
    g_object_unref(app.application);
    g_ptr_array_unref(app.accounts);
    mail_store_close(&app.store);
    g_mime_shutdown();
    return result;
}
