#include "mail.h"

#include <curl/curl.h>
#include <gmime/gmime.h>
#include <libsecret/secret.h>
#include <string.h>

typedef struct {
    const guint8 *data;
    gsize length;
    gsize offset;
} Upload;

static size_t read_upload(char *dst, size_t size, size_t count, void *user)
{
    Upload *upload = user;
    if (size && count > G_MAXSIZE / size) return 0;
    gsize available = upload->length - upload->offset;
    gsize n = MIN((gsize)(size * count), available);
    if (n) memcpy(dst, upload->data + upload->offset, n);
    upload->offset += n;
    return n;
}

static int cancel_transfer(void *user, curl_off_t download_total,
                           curl_off_t download_now, curl_off_t upload_total,
                           curl_off_t upload_now)
{
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return g_cancellable_is_cancelled(user) ? 1 : 0;
}

gboolean mail_network_send_outbox(MailStore *store, const MailAccount *account,
                                  const char *password, GCancellable *cancel, GError **error)
{
    GPtrArray *outbox = mail_store_outbox(store, account->id);
    gboolean ok = TRUE;
    for (guint i = 0; i < outbox->len; ++i) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) {
            ok = FALSE;
            break;
        }
        MailRow *row = g_ptr_array_index(outbox, i);
        if (!row->on_server) continue; /* delivery uncertain; never auto-resend */
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(store->db, "SELECT raw FROM outbox WHERE id=?1",
                               -1, &stmt, NULL) != SQLITE_OK) { ok = FALSE; break; }
        sqlite3_bind_int64(stmt, 1, row->id);
        GBytes *raw = NULL;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            raw = g_bytes_new(sqlite3_column_blob(stmt, 0),
                              (gsize)sqlite3_column_bytes(stmt, 0));
        sqlite3_finalize(stmt);
        if (!raw) { ok = FALSE; break; }
        if (sqlite3_prepare_v2(store->db,
            "UPDATE outbox SET status=1 WHERE id=?1", -1, &stmt, NULL) != SQLITE_OK) {
            g_bytes_unref(raw);
            ok = FALSE;
            break;
        }
        sqlite3_bind_int64(stmt, 1, row->id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) { g_bytes_unref(raw); break; }

        CURL *curl = curl_easy_init();
        if (!curl) {
            g_bytes_unref(raw);
            g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK, "Cannot initialize SMTP client");
            ok = FALSE;
            break;
        }
        const char *scheme = account->smtp_starttls ? "smtp" : "smtps";
        char *url = g_strdup_printf("%s://%s:%u", scheme,
                                    account->smtp_host, account->smtp_port);
        struct curl_slist *recipients = NULL;
        recipients = curl_slist_append(recipients, row->sender);
        Upload upload = { g_bytes_get_data(raw, NULL), g_bytes_get_size(raw), 0 };
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "smtp,smtps");
        curl_easy_setopt(curl, CURLOPT_USERNAME, account->username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (account->smtp_starttls)
            curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, account->address);
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_upload);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK)
            ok = mail_store_mark_sent(store, row->id, error);
        else {
            g_set_error(error, SPACE_ERR, SPACE_ERROR_NETWORK,
                        "Could not confirm delivery to %s: %s. Message remains in Outbox and will not be resent automatically.",
                        row->sender, curl_easy_strerror(result));
            ok = FALSE;
        }
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        g_free(url);
        g_bytes_unref(raw);
        if (!ok) break;
    }
    if (!ok && error && !*error)
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "%s", sqlite3_errmsg(store->db));
    g_ptr_array_unref(outbox);
    return ok;
}

GBytes *mail_compose_raw(const MailAccount *account, const char *recipient,
                         const char *subject, const char *body,
                         const char *attachment_path, GError **error)
{
    if (!recipient || !*recipient || strchr(recipient, '\r') ||
        strchr(recipient, '\n') || !strchr(recipient, '@') ||
        (subject && (strchr(subject, '\r') || strchr(subject, '\n')))) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT, "Enter a valid recipient and subject");
        return NULL;
    }
    GMimeMessage *msg = g_mime_message_new(TRUE);
    g_mime_message_add_mailbox(msg, GMIME_ADDRESS_TYPE_FROM, NULL, account->address);
    g_mime_message_add_mailbox(msg, GMIME_ADDRESS_TYPE_TO, NULL, recipient);
    g_mime_message_set_subject(msg, subject ? subject : "", "UTF-8");
    GDateTime *date = g_date_time_new_now_local();
    g_mime_message_set_date(msg, date);
    g_date_time_unref(date);
    char *id = g_mime_utils_generate_message_id(NULL);
    g_mime_message_set_message_id(msg, id);
    g_free(id);
    GMimeTextPart *part = g_mime_text_part_new_with_subtype("plain");
    g_mime_text_part_set_text(part, body ? body : "");
    char *attachment_data = NULL;
    if (attachment_path) {
        gsize attachment_size = 0;
        if (!g_file_get_contents(attachment_path, &attachment_data,
                                 &attachment_size, error)) {
            g_object_unref(part);
            g_object_unref(msg);
            return NULL;
        }
        if (attachment_size > 32 * 1024 * 1024) {
            g_set_error(error, SPACE_ERR, SPACE_ERROR_INPUT,
                        "Attachment exceeds the 32 MiB limit");
            g_free(attachment_data);
            g_object_unref(part);
            g_object_unref(msg);
            return NULL;
        }
        GMimeMultipart *mixed = g_mime_multipart_new_with_subtype("mixed");
        g_mime_multipart_add(mixed, GMIME_OBJECT(part));
        GMimePart *attachment = g_mime_part_new_with_type("application", "octet-stream");
        char *name = g_path_get_basename(attachment_path);
        g_mime_part_set_filename(attachment, name);
        g_mime_object_set_disposition(GMIME_OBJECT(attachment), GMIME_DISPOSITION_ATTACHMENT);
        GMimeStream *content = g_mime_stream_mem_new_with_buffer(attachment_data, attachment_size);
        GMimeDataWrapper *wrapper = g_mime_data_wrapper_new_with_stream(
            content, GMIME_CONTENT_ENCODING_DEFAULT);
        g_mime_part_set_content(attachment, wrapper);
        g_mime_part_set_content_encoding(attachment, GMIME_CONTENT_ENCODING_BASE64);
        g_mime_multipart_add(mixed, GMIME_OBJECT(attachment));
        g_mime_message_set_mime_part(msg, GMIME_OBJECT(mixed));
        g_object_unref(wrapper);
        g_object_unref(content);
        g_object_unref(attachment);
        g_object_unref(mixed);
        g_free(name);
    } else g_mime_message_set_mime_part(msg, GMIME_OBJECT(part));
    g_object_unref(part);
    GMimeStream *stream = g_mime_stream_mem_new();
    if (g_mime_object_write_to_stream(GMIME_OBJECT(msg), NULL, stream) < 0) {
        g_set_error(error, SPACE_ERR, SPACE_ERROR_STORAGE, "Could not encode message");
        g_object_unref(stream);
        g_object_unref(msg);
        g_free(attachment_data);
        return NULL;
    }
    GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
    GBytes *raw = g_bytes_new(bytes->data, bytes->len);
    g_object_unref(stream);
    g_object_unref(msg);
    g_free(attachment_data);
    return raw;
}
