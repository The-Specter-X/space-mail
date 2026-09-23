#include "../src/mail.h"

#include <gmime/gmime.h>
#include <glib/gstdio.h>
#include <string.h>

static MailStore store;
static char *test_dir;

static const char sample[] =
    "From: Alice <alice@example.net>\r\n"
    "To: Bob <bob@example.org>\r\n"
    "Subject: Alpine project notes\r\n"
    "Date: Tue, 15 Sep 2026 12:00:00 +0000\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: text/plain; charset=UTF-8\r\n"
    "\r\n"
    "An offline search should find this sentence.\r\n";

static void test_import_search_export(void)
{
    char *path = g_build_filename(test_dir, "import.eml", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, sample, -1, &error));
    g_assert_no_error(error);
    g_assert_true(mail_store_import_eml(&store, path, &error));
    g_assert_no_error(error);
    GPtrArray *rows = mail_store_list(&store, "offline search");
    g_assert_cmpuint(rows->len, ==, 1);
    MailRow *row = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(row->folder, ==, "Local Archive");
    GBytes *raw = mail_store_read(&store, row->id);
    gsize length;
    const guint8 *data = g_bytes_get_data(raw, &length);
    char *text = mail_message_preview(data, length);
    g_assert_nonnull(strstr(text, "offline search"));
    g_free(text);
    g_bytes_unref(raw);
    char *export_path = g_build_filename(test_dir, "export.eml", NULL);
    g_assert_true(mail_store_export_eml(&store, row->id, export_path, &error));
    g_assert_no_error(error);
    char *exported = NULL;
    g_assert_true(g_file_get_contents(export_path, &exported, NULL, &error));
    g_assert_cmpstr(exported, ==, sample);
    g_assert_no_error(error);
    g_free(exported);
    g_free(export_path);
    g_ptr_array_unref(rows);
    char *mbox = g_build_filename(test_dir, "archive.mbox", NULL);
    g_assert_true(mail_store_export_mbox(&store, mbox, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(mail_store_import_mbox(&store, mbox, &error), ==, 1);
    g_assert_no_error(error);
    g_free(mbox);
    g_free(path);
}

static void test_retention_and_cleanup(void)
{
    GError *error = NULL;
    g_assert_true(mail_store_message(&store, "work", "INBOX", 55, 42,
                                      (const guint8 *)sample, strlen(sample), &error));
    g_assert_no_error(error);
    g_assert_true(mail_store_has_uid(&store, "work", "INBOX", 55, 42));
    GHashTable *server = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    g_assert_true(mail_store_mark_remote_presence(&store, "work", "INBOX", 55,
                                                   server, &error));
    g_assert_no_error(error);
    g_hash_table_unref(server);
    g_assert_cmpuint(mail_store_missing_count(&store), ==, 1);
    GPtrArray *rows = mail_store_list(&store, "");
    g_assert_cmpuint(rows->len, ==, 3);
    g_ptr_array_unref(rows);
    g_assert_true(mail_store_clean_missing(&store, &error));
    g_assert_no_error(error);
    rows = mail_store_list(&store, "");
    g_assert_cmpuint(rows->len, ==, 2);
    g_ptr_array_unref(rows);
}

static void test_outbox(void)
{
    MailAccount account = { .address = "bob@example.org" };
    GError *error = NULL;
    GBytes *message = mail_compose_raw(&account, "alice@example.net",
                                       "A friendly hello", "Hello!", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(message);
    gsize length;
    const guint8 *raw = g_bytes_get_data(message, &length);
    g_assert_true(mail_store_queue(&store, "work", "alice@example.net", raw,
                                   length, &error));
    g_assert_no_error(error);
    GPtrArray *outbox = mail_store_outbox(&store, "work");
    g_assert_cmpuint(outbox->len, ==, 1);
    gint64 id = ((MailRow *)g_ptr_array_index(outbox, 0))->id;
    g_assert_true(mail_store_mark_sent(&store, id, &error));
    g_assert_no_error(error);
    g_ptr_array_unref(outbox);
    outbox = mail_store_outbox(&store, "work");
    g_assert_cmpuint(outbox->len, ==, 0);
    g_ptr_array_unref(outbox);
    GPtrArray *sent = mail_store_list(&store, "friendly hello");
    g_assert_cmpuint(sent->len, ==, 1);
    g_ptr_array_unref(sent);
    g_bytes_unref(message);
    g_assert_null(mail_compose_raw(&account, "evil@example.net\r\nBcc: bad@example.org",
                                    "hello", "body", NULL, &error));
    g_assert_error(error, SPACE_ERR, SPACE_ERROR_INPUT);
    g_clear_error(&error);
}

static void test_attachment_roundtrip(void)
{
    char *path = g_build_filename(test_dir, "notes.txt", NULL);
    const char payload[] = "offline attachment\nFrom sample";
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, payload, sizeof(payload) - 1, &error));
    MailAccount account = { .address = "bob@example.org" };
    GBytes *raw = mail_compose_raw(&account, "alice@example.net",
                                   "Attachment", "See attached", path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(raw);
    gsize length;
    const guint8 *data = g_bytes_get_data(raw, &length);
    GPtrArray *attachments = mail_message_attachments(data, length);
    g_assert_cmpuint(attachments->len, ==, 1);
    MailAttachment *attachment = g_ptr_array_index(attachments, 0);
    g_assert_cmpstr(attachment->filename, ==, "notes.txt");
    gsize size;
    const char *decoded = g_bytes_get_data(attachment->data, &size);
    g_assert_cmpmem(decoded, size, payload, sizeof(payload) - 1);
    char *text = mail_message_preview(data, length);
    g_assert_cmpstr(text, ==, "See attached");
    g_free(text);
    g_ptr_array_unref(attachments);
    g_bytes_unref(raw);
    g_free(path);
}

static void test_html_only_preview(void)
{
    const char html[] = "From: Alice <alice@example.net>\r\n"
        "Subject: HTML only\r\nMIME-Version: 1.0\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n\r\n"
        "<html><head><style>invisible</style></head><body>"
        "<p>Local &amp; readable</p><script>hidden</script></body></html>";
    char *body = mail_message_preview((const guint8 *)html, strlen(html));
    g_assert_nonnull(strstr(body, "Local & readable"));
    g_assert_null(strstr(body, "invisible"));
    g_assert_null(strstr(body, "hidden"));
    g_free(body);
    GError *error = NULL;
    g_assert_true(mail_store_message(&store, "Local", "Local Archive", 0, 0,
                                     (const guint8 *)html, strlen(html), &error));
    g_assert_no_error(error);
    GPtrArray *rows = mail_store_list(&store, "readable");
    g_assert_cmpuint(rows->len, ==, 1);
    g_ptr_array_unref(rows);
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    test_dir = g_dir_make_tmp("space-mail-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_setenv("XDG_DATA_HOME", test_dir, TRUE);
    g_setenv("XDG_CONFIG_HOME", test_dir, TRUE);
    g_mime_init();
    g_assert_true(mail_store_open(&store, &error));
    g_assert_no_error(error);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mail/import-search-export", test_import_search_export);
    g_test_add_func("/mail/retention-cleanup", test_retention_and_cleanup);
    g_test_add_func("/mail/outbox", test_outbox);
    g_test_add_func("/mail/attachments", test_attachment_roundtrip);
    g_test_add_func("/mail/html-only", test_html_only_preview);
    int result = g_test_run();
    mail_store_close(&store);
    g_mime_shutdown();
    g_free(test_dir);
    return result;
}
