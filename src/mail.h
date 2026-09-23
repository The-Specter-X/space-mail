#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>

#define SPACE_APP_ID "org.axionis.SpaceMail"
#define SPACE_ERR space_error_quark()

typedef enum {
    SPACE_ERROR_STORAGE,
    SPACE_ERROR_NETWORK,
    SPACE_ERROR_AUTH,
    SPACE_ERROR_INPUT
} SpaceError;

GQuark space_error_quark(void);

typedef struct {
    char *id;
    char *address;
    char *username;
    char *imap_host;
    guint imap_port;
    char *smtp_host;
    guint smtp_port;
    gboolean smtp_starttls;
} MailAccount;

typedef struct {
    sqlite3 *db;
    char *data_dir;
} MailStore;

typedef struct {
    gint64 id;
    char *account;
    char *folder;
    char *sender;
    char *subject;
    char *date;
    gboolean on_server;
} MailRow;

typedef struct {
    char *filename;
    GBytes *data;
} MailAttachment;

void mail_attachment_free(gpointer attachment);
GPtrArray *mail_message_attachments(const guint8 *raw, gsize length);
void mail_account_free(gpointer account);
GPtrArray *mail_accounts_load(GError **error);
gboolean mail_account_save(const MailAccount *account, GError **error);
char *mail_password_lookup(const MailAccount *account, GError **error);
gboolean mail_password_save(const MailAccount *account, const char *password, GError **error);

gboolean mail_store_open(MailStore *store, GError **error);
void mail_store_close(MailStore *store);
gboolean mail_store_message(MailStore *store, const char *account, const char *folder,
                           guint64 uidvalidity, guint64 uid, const guint8 *raw,
                           gsize length, GError **error);
gboolean mail_store_has_uid(MailStore *store, const char *account, const char *folder,
                            guint64 uidvalidity, guint64 uid);
gboolean mail_store_queue(MailStore *store, const char *account, const char *recipient,
                          const guint8 *raw, gsize length, GError **error);
GPtrArray *mail_store_outbox(MailStore *store, const char *account);
gboolean mail_store_mark_sent(MailStore *store, gint64 id, GError **error);
gboolean mail_store_retry(MailStore *store, gint64 id, GError **error);
GPtrArray *mail_store_list(MailStore *store, const char *query);
void mail_row_free(gpointer row);
GBytes *mail_store_read(MailStore *store, gint64 id);
gboolean mail_store_mark_remote_presence(MailStore *store, const char *account,
                                          const char *folder, guint64 uidvalidity,
                                          GHashTable *uids, GError **error);
guint mail_store_missing_count(MailStore *store);
gboolean mail_store_clean_missing(MailStore *store, GError **error);
gboolean mail_store_import_eml(MailStore *store, const char *path, GError **error);
guint mail_store_import_mbox(MailStore *store, const char *path, GError **error);
gboolean mail_store_export_mbox(MailStore *store, const char *path, GError **error);
gboolean mail_store_export_eml(MailStore *store, gint64 id, const char *path,
                               GError **error);
char *mail_message_preview(const guint8 *raw, gsize length);

/* These functions may block: call from a worker, never on the GTK main thread. */
gboolean mail_network_sync(MailStore *store, const MailAccount *account,
                           const char *password, guint *new_messages, GCancellable *cancel,
                           GError **error);
gboolean mail_network_send_outbox(MailStore *store, const MailAccount *account,
                                  const char *password, GCancellable *cancel, GError **error);

GBytes *mail_compose_raw(const MailAccount *account, const char *recipient,
                         const char *subject, const char *body,
                         const char *attachment_path, GError **error);
