#include "mail.h"

#include <gio/gio.h>
#include <string.h>

/* A small, bounded subset of IMAP4: LIST, SELECT, UID SEARCH and UID FETCH
 * BODY.PEEK[]. GIO handles TLS and certificate validation. We parse only the
 * exact server responses required for local mirroring. */
typedef struct {
    GSocketConnection *connection;
    GDataInputStream *input;
    GOutputStream *output; /* borrowed from connection */
    GCancellable *cancel;  /* owned by the sync task */
    guint next_tag;
} Imap;

typedef struct {
    GPtrArray *folders;  /* char* */
    GArray *uids;        /* guint64 */
    GByteArray *message;
    guint64 validity;
    gboolean got_search;
} ImapReply;

static void imap_reply_clear(ImapReply *r)
{
    if (r->folders) g_ptr_array_unref(r->folders);
    if (r->uids) g_array_unref(r->uids);
    if (r->message) g_byte_array_unref(r->message);
    memset(r, 0, sizeof *r);
}

static char *read_line(Imap *imap, GError **error)
{
    gsize length = 0;
    char *line = g_data_input_stream_read_line(imap->input, &length, imap->cancel, error);
    if (!line && error && !*error)
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "IMAP server closed the connection");
    if (line && length > 65536) {
        g_free(line);
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "IMAP response line is too long");
        return NULL;
    }
    return line;
}

static char *mailbox_from_list(const char *line)
{
    const char *end = line + strlen(line);
    while (end > line && g_ascii_isspace(end[-1])) --end;
    if (end <= line) return NULL;
    if (end[-1] == '"') {
        const char *opening = end - 1;
        while (opening > line) {
            --opening;
            if (*opening == '"' && (opening == line || opening[-1] != '\\')) break;
        }
        if (*opening != '"') return NULL;
        GString *name = g_string_new(NULL);
        for (const char *p = opening + 1; p < end - 1; ++p) {
            if (*p == '\\' && p + 1 < end - 1) ++p;
            g_string_append_c(name, *p);
        }
        return g_string_free(name, FALSE);
    }
    const char *start = end;
    while (start > line && !g_ascii_isspace(start[-1])) --start;
    return g_strndup(start, end - start);
}

static void parse_search(ImapReply *reply, const char *line)
{
    reply->got_search = TRUE;
    if (!reply->uids) reply->uids = g_array_new(FALSE, FALSE, sizeof(guint64));
    const char *p = line + strlen("* SEARCH");
    while (*p) {
        while (*p == ' ') ++p;
        if (!g_ascii_isdigit(*p)) break;
        char *end = NULL;
        guint64 uid = g_ascii_strtoull(p, &end, 10);
        if (uid && uid <= G_MAXUINT32) g_array_append_val(reply->uids, uid);
        p = end;
    }
}

static gboolean read_literal(Imap *imap, const char *line, ImapReply *reply, GError **error)
{
    const char *end = line + strlen(line);
    if (end <= line || end[-1] != '}') return TRUE;
    const char *start = strrchr(line, '{');
    if (!start || start + 1 == end - 1) return TRUE;
    for (const char *p = start + 1; p < end - 1; ++p)
        if (!g_ascii_isdigit(*p)) return TRUE;
    guint64 length = g_ascii_strtoull(start + 1, NULL, 10);
    if (length > 64 * 1024 * 1024) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "IMAP message exceeds 64 MiB");
        return FALSE;
    }
    if (reply->message) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "Unexpected extra IMAP literal");
        return FALSE;
    }
    reply->message = g_byte_array_sized_new((guint)length + 1);
    g_byte_array_set_size(reply->message, (guint)length);
    gsize received = 0;
    if (length && !g_input_stream_read_all(G_INPUT_STREAM(imap->input),
            reply->message->data, (gsize)length, &received, imap->cancel, error))
        return FALSE;
    return TRUE;
}

static gboolean imap_command(Imap *imap, ImapReply *reply, GError **error,
                             const char *command)
{
    guint tag_number = ++imap->next_tag;
    char *tag = g_strdup_printf("A%04u", tag_number);
    char *request = g_strdup_printf("%s %s\r\n", tag, command);
    gboolean ok = g_output_stream_write_all(imap->output, request,
                       strlen(request), NULL, imap->cancel, error);
    g_free(request);
    if (!ok) { g_free(tag); return FALSE; }

    for (;;) {
        char *line = read_line(imap, error);
        if (!line) { ok = FALSE; break; }
        if (g_str_has_prefix(line, tag) && line[strlen(tag)] == ' ') {
            ok = g_str_has_prefix(line + strlen(tag) + 1, "OK");
            if (!ok)
                g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK,
                            "IMAP command failed: %.150s", line);
            g_free(line);
            break;
        }
        if (g_str_has_prefix(line, "* SEARCH"))
            parse_search(reply, line);
        else if (g_str_has_prefix(line, "* LIST ") &&
                 !strstr(line, "\\Noselect") && !strstr(line, "\\NOSELECT")) {
            char *folder = mailbox_from_list(line);
            if (folder && *folder && g_utf8_validate(folder, -1, NULL)) {
                if (!reply->folders)
                    reply->folders = g_ptr_array_new_with_free_func(g_free);
                if (reply->folders->len < 128) g_ptr_array_add(reply->folders, folder);
                else g_free(folder);
            } else g_free(folder);
        } else if (strstr(line, "UIDVALIDITY ")) {
            const char *p = strstr(line, "UIDVALIDITY ") + strlen("UIDVALIDITY ");
            if (g_ascii_isdigit(*p)) reply->validity = g_ascii_strtoull(p, NULL, 10);
        }
        if (g_str_has_prefix(line, "* ") && strstr(line, " FETCH ") &&
            !read_literal(imap, line, reply, error)) {
            ok = FALSE;
            g_free(line);
            break;
        }
        g_free(line);
    }
    g_free(tag);
    return ok;
}

static char *imap_quote(const char *value)
{
    if (!value || strchr(value, '\n') || strchr(value, '\r')) return NULL;
    GString *out = g_string_new("\"");
    for (const char *p = value; *p; ++p) {
        if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

static gboolean imap_login(Imap *imap, const char *username, const char *password,
                            GError **error)
{
    char *user = imap_quote(username);
    char *pass = imap_quote(password);
    if (!user || !pass) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Login contains an invalid line break");
        g_free(user);
        g_free(pass);
        return FALSE;
    }
    char *command = g_strdup_printf("LOGIN %s %s", user, pass);
    ImapReply reply = { 0 };
    gboolean ok = imap_command(imap, &reply, error, command);
    imap_reply_clear(&reply);
    g_free(command);
    g_free(user);
    g_free(pass);
    return ok;
}

static gboolean imap_connect(Imap *imap, const MailAccount *account,
                              const char *password, GError **error)
{
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_tls(client, TRUE); /* validates CA and hostname by default */
    g_socket_client_set_timeout(client, 45);
#ifdef SPACE_MAIL_TEST_CERT
    GTlsDatabase *trust = g_tls_file_database_new(SPACE_MAIL_TEST_CERT, error);
    if (!trust) { g_object_unref(client); return FALSE; }
    g_tls_backend_set_default_database(g_tls_backend_get_default(), trust);
    g_object_unref(trust);
#endif
    imap->connection = g_socket_client_connect_to_host(client, account->imap_host,
                                                        account->imap_port, imap->cancel, error);
    g_object_unref(client);
    if (!imap->connection) return FALSE;
    imap->input = g_data_input_stream_new(g_io_stream_get_input_stream(
                                           G_IO_STREAM(imap->connection)));
    g_data_input_stream_set_newline_type(imap->input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
    imap->output = g_io_stream_get_output_stream(G_IO_STREAM(imap->connection));
    char *greeting = read_line(imap, error);
    if (!greeting) return FALSE;
    gboolean preauth = g_str_has_prefix(greeting, "* PREAUTH");
    gboolean ok = preauth || g_str_has_prefix(greeting, "* OK");
    g_free(greeting);
    if (!ok) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "Invalid IMAP greeting");
        return FALSE;
    }
    return preauth || imap_login(imap, account->username, password, error);
}

static void imap_disconnect(Imap *imap)
{
    if (imap->connection) {
        if (imap->input) g_object_unref(imap->input);
        g_io_stream_close(G_IO_STREAM(imap->connection), NULL, NULL);
        g_object_unref(imap->connection);
    }
}

static gboolean sync_folder(Imap *imap, MailStore *store, const MailAccount *account,
                            const char *folder, guint *new_messages, GError **error)
{
    char *quoted = imap_quote(folder);
    if (!quoted) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Invalid IMAP mailbox name");
        return FALSE;
    }
    char *command = g_strdup_printf("SELECT %s", quoted);
    g_free(quoted);
    ImapReply reply = { 0 };
    gboolean ok = imap_command(imap, &reply, error, command);
    g_free(command);
    guint64 validity = reply.validity;
    imap_reply_clear(&reply);
    if (ok && !validity) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK,
                    "Server did not return UIDVALIDITY for %s", folder);
        return FALSE;
    }
    if (!ok) return FALSE;

    ok = imap_command(imap, &reply, error, "UID SEARCH ALL");
    if (ok && !reply.got_search) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK,
                    "IMAP SEARCH response was not understood");
        ok = FALSE;
    }
    GHashTable *seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (guint i = 0; ok && reply.uids && i < reply.uids->len; ++i) {
        if (g_cancellable_set_error_if_cancelled(imap->cancel, error)) {
            ok = FALSE;
            break;
        }
        guint64 uid = g_array_index(reply.uids, guint64, i);
        guint64 *key = g_new(guint64, 1);
        *key = uid;
        g_hash_table_add(seen, key);
        if (mail_store_has_uid(store, account->id, folder, validity, uid)) continue;
        char *fetch = g_strdup_printf("UID FETCH %" G_GUINT64_FORMAT " (BODY.PEEK[])", uid);
        ImapReply body = { 0 };
        ok = imap_command(imap, &body, error, fetch);
        g_free(fetch);
        if (ok && (!body.message || !body.message->len)) {
            g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK,
                        "IMAP message %" G_GUINT64_FORMAT " was not returned", uid);
            ok = FALSE;
        }
        if (ok) {
            ok = mail_store_message(store, account->id, folder, validity, uid,
                                    body.message->data, body.message->len, error);
            if (ok) ++*new_messages;
        }
        imap_reply_clear(&body);
    }
    /* Marking a message absent is safe only after a complete folder scan. */
    if (ok) ok = mail_store_mark_remote_presence(store, account->id, folder,
                                                  validity, seen, error);
    g_hash_table_unref(seen);
    imap_reply_clear(&reply);
    return ok;
}

gboolean mail_network_sync(MailStore *store, const MailAccount *account,
                           const char *password, guint *new_messages,
                           GCancellable *cancel, GError **error)
{
    Imap imap = { .cancel = cancel };
    gboolean ok = imap_connect(&imap, account, password, error);
    if (!ok) { imap_disconnect(&imap); return FALSE; }
    ImapReply reply = { 0 };
    ok = imap_command(&imap, &reply, error, "LIST \"\" \"*\"");
    gboolean inbox = FALSE;
    for (guint i = 0; ok && reply.folders && i < reply.folders->len; ++i) {
        const char *folder = g_ptr_array_index(reply.folders, i);
        if (!g_ascii_strcasecmp(folder, "INBOX")) inbox = TRUE;
        ok = sync_folder(&imap, store, account, folder, new_messages, error);
    }
    if (ok && !inbox)
        ok = sync_folder(&imap, store, account, "INBOX", new_messages, error);
    imap_reply_clear(&reply);
    imap_disconnect(&imap);
    return ok;
}
