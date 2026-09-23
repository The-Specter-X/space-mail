#include "mail.h"

#include <errno.h>
#include <fcntl.h>
#include <gmime/gmime.h>
#include <glib/gstdio.h>
#include <libsecret/secret.h>
#include <libxml/HTMLparser.h>
#include <stdio.h>
#include <string.h>

static const SecretSchema mail_secret_schema = {
    .name = "org.axionis.SpaceMail.Password",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = { { "account", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } }
};

GQuark space_error_quark(void)
{
    return g_quark_from_static_string("space-mail-error");
}

void mail_account_free(gpointer data)
{
    MailAccount *a = data;
    if (!a) return;
    g_free(a->id);
    g_free(a->address);
    g_free(a->username);
    g_free(a->imap_host);
    g_free(a->smtp_host);
    g_free(a);
}

static char *account_file(void)
{
    return g_build_filename(g_get_user_config_dir(), SPACE_APP_ID, "accounts.ini", NULL);
}

GPtrArray *mail_accounts_load(GError **error)
{
    GPtrArray *accounts = g_ptr_array_new_with_free_func(mail_account_free);
    char *path = account_file();
    GKeyFile *file = g_key_file_new();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_key_file_free(file);
        g_free(path);
        return accounts;
    }
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, error)) {
        g_key_file_free(file);
        g_free(path);
        g_ptr_array_unref(accounts);
        return NULL;
    }
    gsize count = 0;
    char **groups = g_key_file_get_groups(file, &count);
    for (gsize i = 0; i < count; ++i) {
        MailAccount *a = g_new0(MailAccount, 1);
        a->id = g_strdup(groups[i]);
        a->address = g_key_file_get_string(file, groups[i], "address", NULL);
        a->username = g_key_file_get_string(file, groups[i], "username", NULL);
        a->imap_host = g_key_file_get_string(file, groups[i], "imap_host", NULL);
        a->imap_port = (guint)g_key_file_get_integer(file, groups[i], "imap_port", NULL);
        a->smtp_host = g_key_file_get_string(file, groups[i], "smtp_host", NULL);
        a->smtp_port = (guint)g_key_file_get_integer(file, groups[i], "smtp_port", NULL);
        a->smtp_starttls = g_key_file_get_boolean(file, groups[i], "smtp_starttls", NULL);
        if (a->address && a->username && a->imap_host && a->smtp_host &&
            a->imap_port && a->smtp_port)
            g_ptr_array_add(accounts, a);
        else
            mail_account_free(a);
    }
    g_strfreev(groups);
    g_key_file_free(file);
    g_free(path);
    return accounts;
}

gboolean mail_account_save(const MailAccount *a, GError **error)
{
    char *path = account_file();
    char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot create config directory: %s", g_strerror(errno));
        g_free(dir);
        g_free(path);
        return FALSE;
    }
    GKeyFile *file = g_key_file_new();
    if (g_file_test(path, G_FILE_TEST_EXISTS) &&
        !g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, error)) {
        g_key_file_free(file);
        g_free(dir);
        g_free(path);
        return FALSE;
    }
    g_key_file_set_string(file, a->id, "address", a->address);
    g_key_file_set_string(file, a->id, "username", a->username);
    g_key_file_set_string(file, a->id, "imap_host", a->imap_host);
    g_key_file_set_integer(file, a->id, "imap_port", (int)a->imap_port);
    g_key_file_set_string(file, a->id, "smtp_host", a->smtp_host);
    g_key_file_set_integer(file, a->id, "smtp_port", (int)a->smtp_port);
    g_key_file_set_boolean(file, a->id, "smtp_starttls", a->smtp_starttls);
    gsize len;
    char *contents = g_key_file_to_data(file, &len, error);
    gboolean ok = contents && g_file_set_contents(path, contents, (gssize)len, error);
    if (ok && g_chmod(path, 0600) != 0) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot protect configuration file");
        ok = FALSE;
    }
    g_free(contents);
    g_key_file_free(file);
    g_free(dir);
    g_free(path);
    return ok;
}

char *mail_password_lookup(const MailAccount *account, GError **error)
{
    return secret_password_lookup_sync(&mail_secret_schema, NULL, error,
                                       "account", account->id, NULL);
}

gboolean mail_password_save(const MailAccount *account, const char *password, GError **error)
{
    char *label = g_strdup_printf("Space Mail: %s", account->address);
    gboolean ok = secret_password_store_sync(&mail_secret_schema, SECRET_COLLECTION_DEFAULT,
                                             label, password, NULL, error,
                                             "account", account->id, NULL);
    g_free(label);
    return ok;
}

static gboolean execute(sqlite3 *db, const char *sql, GError **error)
{
    char *detail = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &detail) == SQLITE_OK) return TRUE;
    g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", detail ? detail : sqlite3_errmsg(db));
    sqlite3_free(detail);
    return FALSE;
}

static gboolean step_done(sqlite3 *db, sqlite3_stmt *stmt, GError **error)
{
    if (sqlite3_step(stmt) == SQLITE_DONE) return TRUE;
    g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(db));
    return FALSE;
}

gboolean mail_store_open(MailStore *store, GError **error)
{
    store->data_dir = g_build_filename(g_get_user_data_dir(), SPACE_APP_ID, NULL);
    if (g_mkdir_with_parents(store->data_dir, 0700) != 0) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot create mail data directory: %s", g_strerror(errno));
        return FALSE;
    }
    char *path = g_build_filename(store->data_dir, "mail.sqlite3", NULL);
    int rc = sqlite3_open_v2(path, &store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                              SQLITE_OPEN_FULLMUTEX, NULL);
    g_free(path);
    if (rc != SQLITE_OK) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot open local mail database: %s",
                    sqlite3_errmsg(store->db));
        return FALSE;
    }
    char *dbpath = g_build_filename(store->data_dir, "mail.sqlite3", NULL);
    g_chmod(dbpath, 0600);
    g_free(dbpath);
    sqlite3_busy_timeout(store->db, 5000);
    return execute(store->db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS messages ("
        " id INTEGER PRIMARY KEY, account TEXT NOT NULL, folder TEXT NOT NULL,"
        " uidvalidity INTEGER, uid INTEGER, sender TEXT NOT NULL DEFAULT '',"
        " subject TEXT NOT NULL DEFAULT '', date TEXT NOT NULL DEFAULT '',"
        " body TEXT NOT NULL DEFAULT '', raw BLOB NOT NULL, on_server INTEGER NOT NULL DEFAULT 1,"
        " UNIQUE(account,folder,uidvalidity,uid));"
        "CREATE TABLE IF NOT EXISTS outbox ("
        " id INTEGER PRIMARY KEY, account TEXT NOT NULL, recipient TEXT NOT NULL,"
        " raw BLOB NOT NULL, status INTEGER NOT NULL DEFAULT 0);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS search USING fts5(sender,subject,body);",
        error);
}

void mail_store_close(MailStore *store)
{
    if (store->db) sqlite3_close(store->db);
    g_free(store->data_dir);
    store->db = NULL;
    store->data_dir = NULL;
}

static void html_text(xmlNode *node, GString *text)
{
    for (; node; node = node->next) {
        if (node->type == XML_TEXT_NODE && node->content) {
            g_string_append(text, (const char *)node->content);
            continue;
        }
        if (node->type != XML_ELEMENT_NODE) continue;
        const char *tag = (const char *)node->name;
        if (!g_ascii_strcasecmp(tag, "script") || !g_ascii_strcasecmp(tag, "style") ||
            !g_ascii_strcasecmp(tag, "head")) continue;
        if (!g_ascii_strcasecmp(tag, "br") || !g_ascii_strcasecmp(tag, "p") ||
            !g_ascii_strcasecmp(tag, "div")) g_string_append_c(text, '\n');
        html_text(node->children, text);
        if (!g_ascii_strcasecmp(tag, "p") || !g_ascii_strcasecmp(tag, "div") ||
            !g_ascii_strcasecmp(tag, "li")) g_string_append_c(text, '\n');
    }
}

static char *message_body(GMimeObject *obj, gboolean html)
{
    if (!obj) return g_strdup("");
    if (GMIME_IS_MULTIPART(obj)) {
        GMimeMultipart *mp = GMIME_MULTIPART(obj);
        for (int i = 0; i < g_mime_multipart_get_count(mp); ++i) {
            GMimeObject *child = g_mime_multipart_get_part(mp, i);
            char *text = message_body(child, html);
            if (text[0]) return text;
            g_free(text);
        }
    } else if (GMIME_IS_PART(obj) && !g_mime_part_is_attachment(GMIME_PART(obj))) {
        GMimeContentType *type = g_mime_object_get_content_type(obj);
        if (g_mime_content_type_is_type(type, "text", html ? "html" : "plain")) {
            GMimeDataWrapper *wrapper = g_mime_part_get_content(GMIME_PART(obj));
            if (!wrapper) return g_strdup("");
            GMimeStream *mem = g_mime_stream_mem_new();
            if (g_mime_data_wrapper_write_to_stream(wrapper, mem) < 0) {
                g_object_unref(mem);
                return g_strdup("");
            }
            GByteArray *arr = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(mem));
            const char *charset = g_mime_content_type_get_parameter(type, "charset");
            GError *conversion = NULL;
            char *body = charset ? g_convert((char *)arr->data, arr->len, "UTF-8",
                                             charset, NULL, NULL, &conversion) : NULL;
            g_clear_error(&conversion);
            if (!body) body = g_utf8_make_valid((char *)arr->data, (gssize)arr->len);
            g_object_unref(mem);
            if (html) {
                htmlDocPtr doc = htmlReadMemory(body, (int)strlen(body), NULL, "UTF-8",
                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_RECOVER);
                GString *text = g_string_new(NULL);
                if (doc) {
                    html_text(xmlDocGetRootElement(doc), text);
                    xmlFreeDoc(doc);
                }
                g_free(body);
                body = g_string_free(text, FALSE);
                g_strstrip(body);
            }
            return body;
        }
    }
    return g_strdup("");
}

char *mail_message_preview(const guint8 *raw, gsize length)
{
    GMimeStream *stream = g_mime_stream_mem_new_with_buffer((const char *)raw, length);
    GMimeParser *parser = g_mime_parser_new_with_stream(stream);
    GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);
    GMimeObject *part = message ? g_mime_message_get_mime_part(message) : NULL;
    char *result = message_body(part, FALSE);
    if (!*result) {
        g_free(result);
        result = message_body(part, TRUE);
    }
    if (message) g_object_unref(message);
    g_object_unref(parser);
    g_object_unref(stream);
    return result;
}

void mail_attachment_free(gpointer data)
{
    MailAttachment *attachment = data;
    if (!attachment) return;
    g_free(attachment->filename);
    g_bytes_unref(attachment->data);
    g_free(attachment);
}

static void collect_attachments(GMimeObject *object, GPtrArray *out)
{
    if (!object) return;
    if (GMIME_IS_MULTIPART(object)) {
        GMimeMultipart *parts = GMIME_MULTIPART(object);
        for (int i = 0; i < g_mime_multipart_get_count(parts); ++i)
            collect_attachments(g_mime_multipart_get_part(parts, i), out);
    } else if (GMIME_IS_PART(object)) {
        GMimePart *part = GMIME_PART(object);
        const char *name = g_mime_part_get_filename(part);
        if (!g_mime_part_is_attachment(part) && !name) return;
        GMimeDataWrapper *wrapper = g_mime_part_get_content(part);
        if (!wrapper) return;
        GMimeStream *mem = g_mime_stream_mem_new();
        if (g_mime_data_wrapper_write_to_stream(wrapper, mem) >= 0) {
            GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(mem));
            if (bytes->len <= 64 * 1024 * 1024) {
                MailAttachment *attachment = g_new0(MailAttachment, 1);
                attachment->filename = g_path_get_basename(name ? name : "attachment");
                attachment->data = g_bytes_new(bytes->data, bytes->len);
                g_ptr_array_add(out, attachment);
            }
        }
        g_object_unref(mem);
    }
}

GPtrArray *mail_message_attachments(const guint8 *raw, gsize length)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(mail_attachment_free);
    GMimeStream *stream = g_mime_stream_mem_new_with_buffer((const char *)raw, length);
    GMimeParser *parser = g_mime_parser_new_with_stream(stream);
    GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);
    if (message) {
        collect_attachments(g_mime_message_get_mime_part(message), out);
        g_object_unref(message);
    }
    g_object_unref(parser);
    g_object_unref(stream);
    return out;
}

gboolean mail_store_message(MailStore *store, const char *account, const char *folder,
                           guint64 uidvalidity, guint64 uid, const guint8 *raw,
                           gsize length, GError **error)
{
    if (!raw || !length || length > 64 * 1024 * 1024) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Message is empty or exceeds 64 MiB");
        return FALSE;
    }
    GMimeStream *stream = g_mime_stream_mem_new_with_buffer((const char *)raw, length);
    GMimeParser *parser = g_mime_parser_new_with_stream(stream);
    GMimeMessage *msg = g_mime_parser_construct_message(parser, NULL);
    g_object_unref(parser);
    g_object_unref(stream);
    if (!msg) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Message could not be parsed");
        return FALSE;
    }
    InternetAddressList *from = g_mime_message_get_from(msg);
    char *sender = from ? internet_address_list_to_string(from, NULL, FALSE) : g_strdup("");
    GMimeObject *part = g_mime_message_get_mime_part(msg);
    char *body = message_body(part, FALSE);
    if (!*body) {
        g_free(body);
        body = message_body(part, TRUE);
    }
    const char *subject = g_mime_message_get_subject(msg);
    const char *date = g_mime_object_get_header(GMIME_OBJECT(msg), "Date");
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(store->db,
        "INSERT OR IGNORE INTO messages(account,folder,uidvalidity,uid,sender,subject,date,body,raw,on_server)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", -1, &stmt, NULL);
    gboolean ok = rc == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(stmt, 1, account, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, folder, -1, SQLITE_TRANSIENT);
        if (uid) {
            sqlite3_bind_int64(stmt, 3, (sqlite3_int64)uidvalidity);
            sqlite3_bind_int64(stmt, 4, (sqlite3_int64)uid);
        }
        sqlite3_bind_text(stmt, 5, sender, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, subject ? subject : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, date ? date : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 9, raw, (int)length, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 10, uid != 0);
        ok = step_done(store->db, stmt, error);
        if (ok && sqlite3_changes(store->db)) {
            sqlite3_int64 id = sqlite3_last_insert_rowid(store->db);
            sqlite3_stmt *idx = NULL;
            if (sqlite3_prepare_v2(store->db,
                "INSERT INTO search(rowid,sender,subject,body) VALUES(?1,?2,?3,?4)",
                -1, &idx, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(idx, 1, id);
                sqlite3_bind_text(idx, 2, sender, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(idx, 3, subject ? subject : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(idx, 4, body, -1, SQLITE_TRANSIENT);
                ok = step_done(store->db, idx, error);
            } else {
                g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(store->db));
                ok = FALSE;
            }
            sqlite3_finalize(idx);
        }
    } else {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(store->db));
    }
    sqlite3_finalize(stmt);
    g_object_unref(msg);
    g_free(sender);
    g_free(body);
    return ok;
}

gboolean mail_store_has_uid(MailStore *store, const char *account, const char *folder,
                            guint64 uidvalidity, guint64 uid)
{
    sqlite3_stmt *s = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(store->db,
        "SELECT 1 FROM messages WHERE account=?1 AND folder=?2 AND uidvalidity=?3 AND uid=?4",
        -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, account, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, folder, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)uidvalidity);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)uid);
        found = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    return found;
}

gboolean mail_store_queue(MailStore *store, const char *account, const char *recipient,
                          const guint8 *raw, gsize length, GError **error)
{
    sqlite3_stmt *s = NULL;
    if (length > 64 * 1024 * 1024 ||
        sqlite3_prepare_v2(store->db,
            "INSERT INTO outbox(account,recipient,raw) VALUES(?1,?2,?3)",
            -1, &s, NULL) != SQLITE_OK) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot queue message");
        return FALSE;
    }
    sqlite3_bind_text(s, 1, account, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, recipient, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 3, raw, (int)length, SQLITE_TRANSIENT);
    gboolean ok = step_done(store->db, s, error);
    sqlite3_finalize(s);
    return ok;
}

void mail_row_free(gpointer data)
{
    MailRow *row = data;
    if (!row) return;
    g_free(row->account);
    g_free(row->folder);
    g_free(row->sender);
    g_free(row->subject);
    g_free(row->date);
    g_free(row);
}

GPtrArray *mail_store_outbox(MailStore *store, const char *account)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(mail_row_free);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(store->db,
        "SELECT id,recipient,status FROM outbox WHERE account=?1 ORDER BY id",
        -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, account, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW) {
            MailRow *row = g_new0(MailRow, 1);
            row->id = sqlite3_column_int64(s, 0);
            row->sender = g_strdup((const char *)sqlite3_column_text(s, 1));
            row->on_server = sqlite3_column_int(s, 2) == 0; /* queued versus uncertain */
            g_ptr_array_add(rows, row);
        }
    }
    sqlite3_finalize(s);
    return rows;
}

gboolean mail_store_mark_sent(MailStore *store, gint64 id, GError **error)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(store->db, "SELECT account,raw FROM outbox WHERE id=?1",
                           -1, &s, NULL) != SQLITE_OK) return FALSE;
    sqlite3_bind_int64(s, 1, id);
    gboolean ok = FALSE;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *account = (const char *)sqlite3_column_text(s, 0);
        const guint8 *raw = sqlite3_column_blob(s, 1);
        gsize length = (gsize)sqlite3_column_bytes(s, 1);
        ok = mail_store_message(store, account, "Sent (local)", 0, 0, raw, length, error);
    }
    sqlite3_finalize(s);
    if (!ok) return FALSE;
    if (sqlite3_prepare_v2(store->db, "DELETE FROM outbox WHERE id=?1", -1, &s, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int64(s, 1, id);
    ok = step_done(store->db, s, error);
    sqlite3_finalize(s);
    return ok;
}

gboolean mail_store_retry(MailStore *store, gint64 id, GError **error)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(store->db,
        "UPDATE outbox SET status=0 WHERE id=?1 AND status=1", -1, &s, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_int64(s, 1, id);
    gboolean ok = step_done(store->db, s, error);
    sqlite3_finalize(s);
    return ok;
}

GPtrArray *mail_store_list(MailStore *store, const char *query)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(mail_row_free);
    sqlite3_stmt *s = NULL;
    char *quoted = NULL;
    if (query && *query) {
        GString *term = g_string_new("\"");
        for (const char *p = query; *p; ++p) {
            if (*p == '"') g_string_append_c(term, '"');
            g_string_append_c(term, *p);
        }
        g_string_append_c(term, '"');
        quoted = g_string_free(term, FALSE);
    }
    const char *sql = quoted
        ? "SELECT id,account,folder,sender,subject,date,on_server FROM messages"
          " WHERE id IN (SELECT rowid FROM search WHERE search MATCH ?1) ORDER BY id DESC"
        : "SELECT id,account,folder,sender,subject,date,on_server FROM messages ORDER BY id DESC";
    if (sqlite3_prepare_v2(store->db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (quoted) sqlite3_bind_text(s, 1, quoted, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW) {
            MailRow *row = g_new0(MailRow, 1);
            row->id = sqlite3_column_int64(s, 0);
            row->account = g_strdup((const char *)sqlite3_column_text(s, 1));
            row->folder = g_strdup((const char *)sqlite3_column_text(s, 2));
            row->sender = g_strdup((const char *)sqlite3_column_text(s, 3));
            row->subject = g_strdup((const char *)sqlite3_column_text(s, 4));
            row->date = g_strdup((const char *)sqlite3_column_text(s, 5));
            row->on_server = sqlite3_column_int(s, 6) != 0;
            g_ptr_array_add(rows, row);
        }
    }
    sqlite3_finalize(s);
    g_free(quoted);
    return rows;
}

GBytes *mail_store_read(MailStore *store, gint64 id)
{
    sqlite3_stmt *s = NULL;
    GBytes *bytes = NULL;
    if (sqlite3_prepare_v2(store->db, "SELECT raw FROM messages WHERE id=?1",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW)
            bytes = g_bytes_new(sqlite3_column_blob(s, 0),
                                (gsize)sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    return bytes;
}

gboolean mail_store_mark_remote_presence(MailStore *store, const char *account,
                                          const char *folder, guint64 uidvalidity,
                                          GHashTable *uids, GError **error)
{
    sqlite3_stmt *scan = NULL, *update = NULL;
    sqlite3_stmt *old_epoch = NULL;
    gboolean ok = sqlite3_prepare_v2(store->db,
        "SELECT id,uid FROM messages WHERE account=?1 AND folder=?2 AND uidvalidity=?3",
        -1, &scan, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(store->db,
        "UPDATE messages SET on_server=?1 WHERE id=?2",
        -1, &update, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(scan, 1, account, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(scan, 2, folder, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(scan, 3, (sqlite3_int64)uidvalidity);
        int rc;
        while ((rc = sqlite3_step(scan)) == SQLITE_ROW) {
            guint64 uid = (guint64)sqlite3_column_int64(scan, 1);
            sqlite3_bind_int(update, 1, g_hash_table_contains(uids, &uid));
            sqlite3_bind_int64(update, 2, sqlite3_column_int64(scan, 0));
            if (!step_done(store->db, update, error)) { ok = FALSE; break; }
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
        }
        if (rc != SQLITE_DONE && ok) ok = FALSE;
    }
    if (!ok && error && !*error)
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(store->db));
    sqlite3_finalize(scan);
    sqlite3_finalize(update);
    if (ok) {
        ok = sqlite3_prepare_v2(store->db,
            "UPDATE messages SET on_server=0 WHERE account=?1 AND folder=?2"
            " AND uid IS NOT NULL AND uidvalidity<>?3", -1, &old_epoch, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(old_epoch, 1, account, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(old_epoch, 2, folder, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(old_epoch, 3, (sqlite3_int64)uidvalidity);
            ok = step_done(store->db, old_epoch, error);
        } else
            g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(store->db));
    }
    sqlite3_finalize(old_epoch);
    return ok;
}

guint mail_store_missing_count(MailStore *store)
{
    sqlite3_stmt *s = NULL;
    guint count = 0;
    if (sqlite3_prepare_v2(store->db,
        "SELECT COUNT(*) FROM messages WHERE on_server=0 AND uid IS NOT NULL",
        -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        count = (guint)sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return count;
}

gboolean mail_store_clean_missing(MailStore *store, GError **error)
{
    /* Search entries must be deleted before the corresponding message rows. */
    gboolean ok = execute(store->db, "BEGIN IMMEDIATE;"
        "DELETE FROM search WHERE rowid IN "
        "(SELECT id FROM messages WHERE on_server=0 AND uid IS NOT NULL);"
        "DELETE FROM messages WHERE on_server=0 AND uid IS NOT NULL;"
        "COMMIT;", error);
    if (!ok) sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

gboolean mail_store_import_eml(MailStore *store, const char *path, GError **error)
{
    char *raw = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &raw, &length, error)) return FALSE;
    gboolean ok = mail_store_message(store, "Local", "Local Archive", 0, 0,
                                     (const guint8 *)raw, length, error);
    g_free(raw);
    return ok;
}

guint mail_store_import_mbox(MailStore *store, const char *path, GError **error)
{
    GMimeStream *stream = g_mime_stream_fs_open(path, O_RDONLY, 0, error);
    if (!stream) return 0;
    GMimeParser *parser = g_mime_parser_new_with_stream(stream);
    g_object_unref(stream);
    g_mime_parser_set_format(parser, GMIME_FORMAT_MBOX);
    guint imported = 0;
    while (!g_mime_parser_eos(parser)) {
        GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);
        if (!message) break;
        GMimeStream *mem = g_mime_stream_mem_new();
        if (g_mime_object_write_to_stream(GMIME_OBJECT(message), NULL, mem) >= 0) {
            GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(mem));
            if (!mail_store_message(store, "Local", "Local Archive", 0, 0,
                                    bytes->data, bytes->len, error)) {
                g_object_unref(mem);
                g_object_unref(message);
                break;
            }
            ++imported;
        }
        g_object_unref(mem);
        g_object_unref(message);
    }
    g_object_unref(parser);
    if (!imported && error && !*error)
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "No messages found in mbox");
    return imported;
}

gboolean mail_store_export_mbox(MailStore *store, const char *path, GError **error)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Cannot open archive: %s", g_strerror(errno));
        return FALSE;
    }
    GPtrArray *rows = mail_store_list(store, "");
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < rows->len; ++i) {
        MailRow *row = g_ptr_array_index(rows, i);
        GBytes *raw = mail_store_read(store, row->id);
        if (!raw) { ok = FALSE; break; }
        gsize length;
        const guint8 *data = g_bytes_get_data(raw, &length);
        if (fputs("From - Thu Jan  1 00:00:00 1970\n", file) < 0) ok = FALSE;
        gboolean beginning = TRUE;
        for (gsize j = 0; ok && j < length; ++j) {
            /* mboxrd quotes a line containing From, including previously
             * quoted >From. Handle both CRLF and LF source messages. */
            if (beginning) {
                gsize k = j;
                while (k < length && data[k] == '>') ++k;
                if (k + 5 <= length && memcmp(data + k, "From ", 5) == 0 &&
                    fputc('>', file) == EOF) { ok = FALSE; break; }
                beginning = FALSE;
            }
            if (data[j] == '\r' && j + 1 < length && data[j + 1] == '\n') continue;
            if (fputc(data[j], file) == EOF) { ok = FALSE; break; }
            if (data[j] == '\n') beginning = TRUE;
        }
        if (ok && (length == 0 || data[length - 1] != '\n') && fputc('\n', file) == EOF)
            ok = FALSE;
        if (ok && fputc('\n', file) == EOF) ok = FALSE;
        g_bytes_unref(raw);
    }
    g_ptr_array_unref(rows);
    if (fclose(file) != 0) ok = FALSE;
    if (!ok)
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Could not write complete mbox archive");
    return ok;
}

gboolean mail_store_export_eml(MailStore *store, gint64 id, const char *path, GError **error)
{
    GBytes *bytes = mail_store_read(store, id);
    if (!bytes) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Select a message to export");
        return FALSE;
    }
    gsize length;
    const char *raw = g_bytes_get_data(bytes, &length);
    gboolean ok = g_file_set_contents(path, raw, (gssize)length, error);
    g_bytes_unref(bytes);
    return ok;
}
