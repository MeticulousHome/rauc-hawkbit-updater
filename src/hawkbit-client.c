/**
 * SPDX-License-Identifier: LGPL-2.1-only
 * SPDX-FileCopyrightText: 2021-2022 Bastian Krause <bst@pengutronix.de>, Pengutronix
 * SPDX-FileCopyrightText: 2018-2020 Lasse K. Mikkelsen <lkmi@prevas.dk>, Prevas A/S (www.prevas.com)
 *
 * @file
 * @brief Implementation of the hawkBit DDI API
 *
 * @see https://github.com/rauc/rauc-hawkbit
 * @see https://eclipse.dev/hawkbit/apis/ddi_api/
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <curl/curl.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libgen.h>
#include <gio/gio.h>
#include <sys/reboot.h>
#include <math.h>

#include "json-helper.h"
#ifdef WITH_SYSTEMD
#include "sd-helper.h"
#include <systemd/sd-daemon.h>
#endif

#include "hawkbit-client.h"
#include "download-progress-dbus.h"

static GDBusConnection *dbus_connection = NULL;
static DownloadProgressDownloadProgress *dbus_interface = NULL;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, fclose)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CURL, curl_easy_cleanup)

gboolean run_once = FALSE;

static const gint MAX_RETRIES_ON_API_ERROR = 10;

/**
 * @brief String representation of HTTP methods.
 */
static const char *HTTPMethod_STRING[] = {
        "GET", "HEAD", "PUT", "POST", "PATCH", "DELETE"
};

/**
 * @brief CURLcodes that should lead to download resuming, 0 terminated
 */
static const gint resumable_codes[] = {
        CURLE_OPERATION_TIMEDOUT,
        CURLE_COULDNT_RESOLVE_HOST,
        CURLE_COULDNT_CONNECT,
        CURLE_PARTIAL_FILE,
        CURLE_SEND_ERROR,
        CURLE_RECV_ERROR,
        CURLE_HTTP2,
        CURLE_HTTP2_STREAM,
        0
};

static Config *hawkbit_config = NULL;
static GSourceFunc software_ready_cb;
static struct HawkbitAction *active_action = NULL;
static GThread *thread_download = NULL;

static void process_deployment_cleanup(void);
static gboolean feedback_progress(const gchar *url, const gchar *id, const gchar *detail, GError **error);

GQuark rhu_hawkbit_client_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_error_quark");
}

GQuark rhu_hawkbit_client_curl_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_curl_error_quark");
}

GQuark rhu_hawkbit_client_http_error_quark(void)
{
        return g_quark_from_static_string("rhu_hawkbit_client_http_error_quark");
}

struct progress{
        GDBusConnection *connection;
        gchar *object_path;
        curl_off_t resume_from;
        gchar *feedback_url;
        gchar *action_id;
};

static gboolean report_download_progress(const gchar *feedback_url, const gchar *action_id, 
                                         double percentage, GError **error) 
{
        g_autofree gchar *msg = NULL;
        
        g_return_val_if_fail(feedback_url, FALSE);
        g_return_val_if_fail(action_id, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
        
        msg = g_strdup_printf("Download progress: %.1f%%", percentage);
        
        return feedback_progress(feedback_url, action_id, msg, error);
}

static size_t progress_callback(void *clientp,
                                curl_off_t dltotal,
                                curl_off_t dlnow,
                                curl_off_t ultotal,
                                curl_off_t ulnow)
{
        struct progress *prog = (struct progress *)clientp;
        static double last_percentage = 0;
        curl_off_t total_size = prog->resume_from + dltotal;
        curl_off_t current_size = prog->resume_from + dlnow;
    
        double percentage;
        if (total_size > 0 && dltotal > 0) {
                percentage = ((double)current_size / (double)total_size) * 100;
        } else {
                percentage = 0;
        }

        if (fabs(percentage - last_percentage) >= 1.0) {
                if (dbus_interface) {
                        download_progress_download_progress_emit_progress_update(dbus_interface, percentage);
                }
                g_print("Download progress: %.1f%%\n", percentage);
                
                if (prog->feedback_url && prog->action_id) {
                        GError *error = NULL;
                        if (!report_download_progress(prog->feedback_url, prog->action_id, percentage, &error)) {
                                g_warning("Failed to report download progress: %s", error->message);
                                g_clear_error(&error);
                        }
                }
    
                last_percentage = percentage;
        }

        return 0;
}

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data)
{
    GError *error = NULL;

    g_print("D-Bus connection acquired\n");

    dbus_connection = connection;
    dbus_interface = download_progress_download_progress_skeleton_new();

    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(dbus_interface),
                                          connection,
                                          "/org/hawkbit/DownloadProgress",
                                          &error)) {
        g_warning("Error exporting object: %s\n", error->message);
        g_error_free(error);
    }
}

static void init_dbus_service(void)
{
    g_bus_own_name(G_BUS_TYPE_SYSTEM,
                   "org.hawkbit.DownloadProgress",
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus_acquired,
                   NULL,
                   NULL,
                   NULL,
                   NULL);
}

/**
 * @brief Create and initialize an HawkbitAction.
 *
 * @return Pointer to initialized HawkbitAction..
 */
static struct HawkbitAction *action_new(void)
{
        struct HawkbitAction *action = g_new0(struct HawkbitAction, 1);

        g_mutex_init(&action->mutex);
        g_cond_init(&action->cond);
        action->id = NULL;
        action->state = ACTION_STATE_NONE;

        return action;
}

/**
 * @brief Get available free space of a mounted file system.
 *
 * @param[in]  path       Absolute Path of a disk device node containing a mounted file system
 * @param[out] free_space Pointer to goffset containing the free space in bytes
 * @return TRUE if free space calculation succeeded, FALSE otherwise
 */
static gboolean get_available_space(const char *path, goffset *free_space, GError **error)
{
        struct statvfs stat;
        g_autofree gchar *npath = g_strdup(path);
        const char *rpath = dirname(npath);

        g_return_val_if_fail(path, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        if (statvfs(rpath, &stat)) {
                int err = errno;
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                            "Failed to calculate free space for %s: %s", path, g_strerror(err));
                return FALSE;
        }

        // the available free space is f_bsize * f_bavail
        *free_space = (goffset) stat.f_bsize * (goffset) stat.f_bavail;
        return TRUE;
}

/**
 * @brief Calculate checksum for file.
 *
 * @param[in]  fp       File to read data from
 * @param[in]  type     Desired type of checksum
 * @param[out] checksum Calculated checksum digest hex string
 * @param[out] error    Error
 * @return TRUE if checksum calculation succeeded, FALSE otherwise (error set)
 */
static gboolean get_file_checksum(FILE *fp, const GChecksumType type, gchar **checksum,
                                  gsize max_bytes, GError **error)
{
        g_autoptr(GChecksum) ctx = g_checksum_new(type);
        guchar buf[4096];
        size_t r;
        gsize bytes_read = 0;
        long initial_position;

        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
        g_return_val_if_fail(fp, FALSE);
        g_return_val_if_fail(checksum && *checksum == NULL, FALSE);
        
        g_assert_nonnull(ctx);

        // Save the initial file position
        initial_position = ftell(fp);
        if (initial_position < 0) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to get file position: %s", g_strerror(errno));
                return FALSE;
        }

        g_debug("Calculating checksum from position %ld, max_bytes: %zu", initial_position, max_bytes);

        while (bytes_read < max_bytes) {
                size_t to_read = MIN(sizeof(buf), max_bytes - bytes_read);
                r = fread(buf, 1, to_read, fp);
                if (ferror(fp)) {
                        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Read failed: %s", g_strerror(errno));
                        // Try to restore the original file position
                        if (fseek(fp, initial_position, SEEK_SET) != 0) {
                                g_warning("Failed to restore file position after read error: %s", g_strerror(errno));
                        }
                        return FALSE;
                }

                g_checksum_update(ctx, buf, r);
                bytes_read += r;

                if (feof(fp))
                        break;
        }

        *checksum = g_strdup(g_checksum_get_string(ctx));

        g_debug("Checksum calculated. Bytes read: %zu, Checksum: %s", bytes_read, *checksum);

        // Restore the original file position
        if (fseek(fp, initial_position, SEEK_SET) != 0) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to restore file position: %s", g_strerror(errno));
                return FALSE;
        }

        return TRUE;
}


/**
 * @brief Add string to Curl headers, avoiding overwriting an existing
 *        non-empty list on failure.
 *
 * @param[out] headers curl_slist** of already set headers
 * @param[in]  string  Header to add
 * @param[out] error   Error
 * @return TRUE if string was added to headers successfully, FALSE otherwise (error set)
 */
static gboolean add_curl_header(struct curl_slist **headers, const char *string, GError **error)
{
        struct curl_slist *temp = NULL;

        g_return_val_if_fail(headers, FALSE);
        g_return_val_if_fail(string, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        temp = curl_slist_append(*headers, string);
        if (!temp) {
                curl_slist_free_all(*headers);
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Could not add header %s", string);
                return FALSE;
        }

        *headers = temp;
        return TRUE;
}

/**
 * @brief Returns the header required to authenticate against hawkBit, either target or gateway
 *        token.
 *
 * @return header required to authenticate against hawkBit
 */
static char* get_auth_header()
{
        if (hawkbit_config->auth_token)
                return g_strdup_printf("Authorization: TargetToken %s",
                                       hawkbit_config->auth_token);

        if (hawkbit_config->gateway_token)
                return g_strdup_printf("Authorization: GatewayToken %s",
                                       hawkbit_config->gateway_token);

        g_return_val_if_reached(NULL);
}

/**
 * @brief Add hawkBit authorization header to Curl headers.
 *
 * @param[out] headers curl_slist** of already set headers
 * @param[out] error   Error
 * @return TRUE if authorization method set in config and header was added successfully, TRUE if no
 *         authorization method set, FALSE otherwise (error set)
 */
static gboolean set_auth_curl_header(struct curl_slist **headers, GError **error)
{
        gboolean res = TRUE;
        g_autofree gchar *token = NULL;

        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        token = get_auth_header();
        if (token)
                res = add_curl_header(headers, token, error);

        return res;
}

/**
 * @brief Set common Curl options, namely user agent, connect timeout, SSL
 *        verify peer and SSL verify host options.
 *
 * @param[in] curl Curl handle
 */
static void set_default_curl_opts(CURL *curl)
{
        g_return_if_fail(curl);

        curl_easy_setopt(curl, CURLOPT_USERAGENT, HAWKBIT_USERAGENT);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, hawkbit_config->connect_timeout);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, hawkbit_config->ssl_verify ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, hawkbit_config->ssl_verify ? 1L : 0L);
}

/**
 * @brief Download download_url to file.
 *
 * @param[in]  download_url URL to download from
 * @param[in]  file         Download destination
 * @param[in]  resume_from  Offset to resume download from
 * @param[out] sha1sum      Calculated checksum or NULL
 * @param[out] speed        Average download speed
 * @param[out] error        Error
 * @return TRUE if download succeeded, FALSE otherwise (error set)
 */
static gboolean get_binary(const gchar *download_url, const gchar *file, curl_off_t resume_from,
                           const gchar *expected_sha1sum, gchar **calculated_sha1sum, 
                           curl_off_t *speed, const gchar *feedback_url, const gchar *action_id,
                           GError **error)
{
        g_autoptr(CURL) curl = NULL;
        FILE *fp = NULL;
        CURLcode curl_code;
        glong http_code = 0;
        struct curl_slist *headers = NULL;
        struct progress prog;
        g_autofree gchar *partial_sha1sum = NULL;
        gsize file_size = 0;

        prog.connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
        if(prog.connection == NULL){
                g_free(prog.feedback_url);
                g_free(prog.action_id);
                return FALSE;
        }
        prog.object_path = "/org/hawkbit/DownloadProgress";
        prog.resume_from = resume_from;
        prog.feedback_url = g_strdup(feedback_url);
        prog.action_id = g_strdup(action_id);

        g_return_val_if_fail(download_url, FALSE);
        g_return_val_if_fail(file, FALSE);
        g_return_val_if_fail(calculated_sha1sum && *calculated_sha1sum == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        if (resume_from)
                g_debug("Resuming download from offset %" CURL_FORMAT_CURL_OFF_T, resume_from);

        // Check if file exists and calculate its partial checksum
        if (g_file_test(file, G_FILE_TEST_EXISTS)) {
                fp = g_fopen(file, "a+b");
                if (fp) {
                        fseek(fp, 0, SEEK_END);
                        file_size = ftell(fp);
                        fseek(fp, 0, SEEK_SET);
                        if (file_size > 0) {
                                g_debug("Calculating partial checksum for existing file");
                                if (get_file_checksum(fp, G_CHECKSUM_SHA1, &partial_sha1sum, file_size, error)) {
                                        resume_from = file_size;
                                        g_debug("Partial checksum calculated. Size: %" G_GSIZE_FORMAT " bytes, Checksum: %s", file_size, partial_sha1sum);
                
                                        // Add this log to print the partial checksum.
                                        g_debug("Partial SHA1 checksum: %s", partial_sha1sum);
                
                                } else {
                                        g_warning("Failed to calculate partial checksum: %s", (*error)->message);
                                        g_clear_error(error);
                                        fclose(fp);
                                        fp = NULL;
                                }
                        } else {
                                g_debug("Existing file is empty. Starting new download.");
                                fclose(fp);
                                fp = NULL;
                        }
                } else {
                        g_warning("Failed to open existing file for checksum calculation: %s", g_strerror(errno));
                }
        }

        if (!fp) {
                g_debug("Opening file for writing: %s", file);
                fp = g_fopen(file, "wb");
                if (!fp) {
                        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "Failed to open %s for download: %s", file, g_strerror(errno));
                        return FALSE;
                }
                resume_from = 0;
        }

        if (resume_from > 0) {
                if (fseek(fp, resume_from, SEEK_SET) != 0) {
                        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                "Failed to seek to position %" CURL_FORMAT_CURL_OFF_T ": %s", 
                                resume_from, g_strerror(errno));
                        fclose(fp);
                        return FALSE;
                }
                g_debug("File pointer set to resume position: %" CURL_FORMAT_CURL_OFF_T, resume_from);
        }

        curl = curl_easy_init();
        if (!curl) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Unable to start libcurl easy session");
                fclose(fp);
                return FALSE;
        }

        set_default_curl_opts(curl);

        if (resume_from > 0) {
                curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
                g_debug("Setting CURLOPT_RESUME_FROM_LARGE to %" CURL_FORMAT_CURL_OFF_T, resume_from);
        }

        prog.resume_from = resume_from;

        curl_easy_setopt(curl, CURLOPT_URL, download_url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, hawkbit_config->low_speed_time);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, hawkbit_config->low_speed_rate);

        if (!set_auth_curl_header(&headers, error)) {
                fclose(fp);
                return FALSE;
}

        if (!add_curl_header(&headers, "Accept: application/octet-stream", error)) {
                fclose(fp);
                return FALSE;
}

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        g_debug("Starting download from %s", download_url);
        curl_code = curl_easy_perform(curl);

        if (curl_code != CURLE_OK) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, curl_code,
                "Download failed: %s", curl_easy_strerror(curl_code));
                fclose(fp);
                curl_slist_free_all(headers);
                return FALSE;
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, speed);
        curl_slist_free_all(headers);

        if (http_code != 200 && http_code != 206 && http_code != 416) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, http_code,
                "HTTP request failed: %ld", http_code);

                g_free(prog.feedback_url);
                g_free(prog.action_id);
                g_object_unref(prog.connection);
                
                fclose(fp);
                return FALSE;
        }

        // Close the file after download
        fclose(fp);
        fp = NULL;

        g_debug("Download completed. Opening file for checksum calculation.");

        // Reopen the file for checksum calculation
        fp = g_fopen(file, "rb");
        if (!fp) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to open %s for checksum calculation: %s", file, g_strerror(errno));
                return FALSE;
        }

        // Get the file size
        fseek(fp, 0, SEEK_END);
        file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        g_debug("Calculating checksum for entire file. Size: %zu bytes", file_size);

        // Calculate checksum for the entire file
        if (!get_file_checksum(fp, G_CHECKSUM_SHA1, calculated_sha1sum, file_size, error)) {
                fclose(fp);
                process_deployment_cleanup();
                return FALSE;
        }

        fclose(fp);

        if (expected_sha1sum && g_strcmp0(*calculated_sha1sum, expected_sha1sum) != 0) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR, RHU_HAWKBIT_CLIENT_ERROR_CHECKSUM,
                        "Checksum mismatch. Expected: %s, Calculated: %s",
                        expected_sha1sum, *calculated_sha1sum);
                process_deployment_cleanup();
                return FALSE;
        }

        g_debug("Download and checksum verification completed successfully.");
        g_debug("Calculated SHA1: %s", *calculated_sha1sum);
        g_free(prog.feedback_url);
        g_free(prog.action_id);
        g_object_unref(prog.connection);
        return TRUE;
}

/**
 * @brief Curl callback writing REST response to RestPayload*->payload buffer.
 *
 * @see   https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
static size_t curl_write_cb(const void *content, size_t size, size_t nmemb, void *data)
{
        RestPayload *p = NULL;
        size_t real_size = size * nmemb;

        g_return_val_if_fail(content, 0);
        g_return_val_if_fail(data, 0);

        p = (RestPayload *) data;
        p->payload = (gchar *) g_realloc(p->payload, p->size + real_size + 1);

        // copy content to buffer
        memcpy(&(p->payload[p->size]), content, real_size);
        p->size += real_size;
        p->payload[p->size] = '\0';

        return real_size;
}

/**
 * @brief Perform REST request with JSON data, expecting response JSON data.
 *
 * @param[in]  method             HTTP Method, e.g. GET
 * @param[in]  url                URL used in HTTP REST request
 * @param[in]  jsonRequestBody    REST request body. If NULL, no body is sent
 * @param[out] jsonResponseParser Return location for a REST response or NULL to skip response
 *                                parsing
 * @param[out] error              Error
 * @return TRUE if request and response parser (if given) suceeded, FALSE otherwise (error set).
 */
static gboolean rest_request(enum HTTPMethod method, const gchar *url,
                             JsonBuilder *jsonRequestBody, JsonParser **jsonResponseParser,
                             GError **error)
{
        g_autofree gchar *postdata = NULL;
        g_autoptr(RestPayload) fetch_buffer = NULL;
        struct curl_slist *headers = NULL;
        g_autoptr(CURL) curl = NULL;
        glong http_code = 0;
        CURLcode res;

        g_return_val_if_fail(url, FALSE);
        g_return_val_if_fail(jsonResponseParser == NULL || *jsonResponseParser == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        curl = curl_easy_init();
        if (!curl) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, CURLE_FAILED_INIT,
                            "Unable to start libcurl easy session");
                return FALSE;
        }

        // init response buffer
        fetch_buffer = g_new0(RestPayload, 1);
        fetch_buffer->size = 0;
        fetch_buffer->payload = g_malloc0(DEFAULT_CURL_REQUEST_BUFFER_SIZE);

        // set up CURL options
        set_default_curl_opts(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, HTTPMethod_STRING[method]);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, hawkbit_config->timeout);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fetch_buffer);

        if (jsonRequestBody) {
                g_autoptr(JsonGenerator) generator = json_generator_new();
                g_autoptr(JsonNode) req_root = json_builder_get_root(jsonRequestBody);
                g_autofree gchar *json_req_str = NULL;

                json_generator_set_root(generator, req_root);
                postdata = json_generator_to_data(generator, NULL);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);
                json_req_str = json_to_string(req_root, TRUE);
                g_debug("Request body: %s", json_req_str);
        }

        // set up request headers
        if (!add_curl_header(&headers, "Accept: application/json;charset=UTF-8", error))
                return FALSE;

        if (!set_auth_curl_header(&headers, error))
                return FALSE;

        if (jsonRequestBody &&
            !add_curl_header(&headers, "Content-Type: application/json;charset=UTF-8", error))
                return FALSE;

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // perform request
        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        if (res != CURLE_OK) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, res, "%s",
                            curl_easy_strerror(res));
                return FALSE;
        }
        if (http_code != 200) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, http_code,
                            "HTTP request failed: %ld; server response: %s", http_code,
                            fetch_buffer->payload);
                return FALSE;
        }

        if (jsonResponseParser && fetch_buffer->size > 0) {
                // process JSON repsonse
                g_autoptr(JsonParser) parser = json_parser_new_immutable();
                JsonNode *resp_root = NULL;
                g_autofree gchar *json_resp_str = NULL;

                if (!json_parser_load_from_data(parser, fetch_buffer->payload, fetch_buffer->size,
                                                error))
                        return FALSE;

                resp_root = json_parser_get_root(parser);
                json_resp_str = json_to_string(resp_root, TRUE);
                g_debug("Response body: %s", json_resp_str);
                *jsonResponseParser = g_steal_pointer(&parser);
        }

        return TRUE;
}

/**
 * @brief Perform REST request with JSON data, expecting response JSON data. On HTTP error
 * 409 (Conflict) and 429 (Too Many Requests), try again (up to MAX_RETRIES_ON_API_ERROR).
 *
 * @param[in]  method             HTTP Method, e.g. GET
 * @param[in]  url                URL used in HTTP REST request
 * @param[in]  jsonRequestBody    REST request body. If NULL, no body is sent
 * @param[out] jsonResponseParser Return location for a REST response or NULL to skip response
 *                                parsing
 * @param[out] error              Error
 * @return TRUE if request and response parser (if given) suceeded, FALSE otherwise (error set).
 */
static gboolean rest_request_retriable(enum HTTPMethod method, const gchar *url,
                                       JsonBuilder *jsonRequestBody,
                                       JsonParser **jsonResponseParser, GError **error)
{
        gboolean res, retry;
        gint retry_count = 0;
        GError *ierror = NULL;

        g_return_val_if_fail(url, FALSE);
        g_return_val_if_fail(jsonResponseParser == NULL || *jsonResponseParser == NULL, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        while (1) {
                res = rest_request(method, url, jsonRequestBody, jsonResponseParser, &ierror);
                retry = (g_error_matches(ierror, RHU_HAWKBIT_CLIENT_HTTP_ERROR, 409) ||
                         g_error_matches(ierror, RHU_HAWKBIT_CLIENT_HTTP_ERROR, 429)) &&
                        retry_count < MAX_RETRIES_ON_API_ERROR;
                if (!retry)
                        break;

                g_debug("%s Trying again (%d/%d)..", ierror->message, retry_count+1,
                        MAX_RETRIES_ON_API_ERROR);
                g_clear_error(&ierror);
                g_usleep(1000000);
                retry_count++;
        }

        if (!res)
                g_propagate_error(error, ierror);

        return res;
}

/**
 * @brief Build hawkBit JSON request.
 *
 * @see https://eclipse.dev/hawkbit/rest-api/rootcontroller-api-guide.html#_post_tenantcontrollerv1controlleriddeploymentbaseactionidfeedback
 *
 * @param[in] id         hawkBit action ID or NULL (configData usecase)
 * @param[in] detail     Detail message or NULL (configData usecase)
 * @param[in] finished   hawkBit status of the result
 * @param[in] execution  hawkBit status of the action execution
 * @param[in] attributes hawkBit controller attributes or NULL (feedback usecase)
 * @return JsonBuilder* with built hawkBit request
 */
static JsonBuilder* json_build_status(const gchar *id, const gchar *detail, const gchar *finished,
                                      const gchar *execution, GHashTable *attributes)
{
        GHashTableIter iter;
        gpointer key, value;
        g_autoptr(JsonBuilder) builder = NULL;
        time_t current_time;
        struct tm time_info;
        char timeString[16];

        g_return_val_if_fail(finished, NULL);
        g_return_val_if_fail(execution, NULL);

        // get current time in UTC
        time(&current_time);
        gmtime_r(&current_time, &time_info);
        strftime(timeString, sizeof(timeString), "%Y%m%dT%H%M%S", &time_info);

        builder = json_builder_new();

        // build json status
        json_builder_begin_object(builder);

        if (id) {
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, id);
        }

        json_builder_set_member_name(builder, "time");
        json_builder_add_string_value(builder, timeString);

        json_builder_set_member_name(builder, "status");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "result");
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "finished");
        json_builder_add_string_value(builder, finished);
        json_builder_end_object(builder);

        json_builder_set_member_name(builder, "execution");
        json_builder_add_string_value(builder, execution);

        if (detail) {
                json_builder_set_member_name(builder, "details");
                json_builder_begin_array(builder);
                json_builder_add_string_value(builder, detail);
                json_builder_end_array(builder);
        }
        json_builder_end_object(builder);

        if (attributes) {
                json_builder_set_member_name(builder, "data");
                json_builder_begin_object(builder);
                g_hash_table_iter_init(&iter, attributes);
                while (g_hash_table_iter_next(&iter, &key, &value)) {
                        json_builder_set_member_name(builder, key);
                        json_builder_add_string_value(builder, value);
                }
                json_builder_end_object(builder);
        }
        json_builder_end_object(builder);

        return g_steal_pointer(&builder);
}

/**
 * @brief Send feedback to hawkBit.
 *
 * @param[in]  url        hawkBit URL used for request
 * @param[in]  id         hawkBit action ID
 * @param[in]  detail     Detail message
 * @param[in]  finished   hawkBit status of the result
 * @param[in]  execution  hawkBit status of the action execution
 * @param[out] error      Error
 * @return TRUE if feedback was sent successfully, FALSE otherwise (error set)
 */
static gboolean feedback(const gchar *url, const gchar *id, const gchar *detail,
                         const gchar *finished, const gchar *execution, GError **error)
{
        g_autoptr(JsonBuilder) builder = NULL;
        gboolean res = FALSE;

        g_return_val_if_fail(url, FALSE);
        g_return_val_if_fail(id, FALSE);
        g_return_val_if_fail(detail, FALSE);
        g_return_val_if_fail(finished, FALSE);
        g_return_val_if_fail(execution, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        if (!g_strcmp0(finished, "failure"))
                g_warning("%s", detail);
        else
                g_message("%s", detail);

        builder = json_build_status(id, detail, finished, execution, NULL);

        res = rest_request_retriable(POST, url, builder, NULL, error);
        if (!res)
                g_prefix_error(error, "Failed to report \"%s\" feedback: ", detail);

        return res;
}

/**
 * @brief Send progress feedback to hawkBit (finished=none, execution=proceeding).
 *
 * @param[in]  url    hawkBit URL used for request
 * @param[in]  id     hawkBit action ID
 * @param[in]  detail Detail message
 * @param[out] error  Error
 * @return TRUE if feedback was sent successfully, FALSE otherwise (error set)
 */
static gboolean feedback_progress(const gchar *url, const gchar *id, const gchar *detail,
                                  GError **error)
{
        gboolean res = FALSE;

        g_return_val_if_fail(url, FALSE);
        g_return_val_if_fail(id, FALSE);
        g_return_val_if_fail(detail, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        res = feedback(url, id, detail, "none", "proceeding", error);
        g_prefix_error(error, "Progress feedback: ");

        return res;
}

/**
 * @brief Get polling sleep time from hawkBit JSON response.
 *
 * @param[in] root JsonNode* with hawkBit response
 * @return time to sleep in seconds, either from JSON or (if not found) from config's retry_wait (or 5s during active action)
 */
static long json_get_sleeptime(JsonNode *root)
{
        g_autofree gchar *sleeptime_str = NULL;
        g_autoptr(GError) error = NULL;
        struct tm time;

        g_return_val_if_fail(root, 0L);

        /* When processing an action, return fixed sleeptime of 5s to allow
         * receiving cancelation requests etc.*/
        g_mutex_lock(&active_action->mutex);
        if (active_action->state == ACTION_STATE_PROCESSING ||
            active_action->state == ACTION_STATE_DOWNLOADING ||
            active_action->state == ACTION_STATE_CANCEL_REQUESTED) {
                g_mutex_unlock(&active_action->mutex);
                return 5L;
        }
        g_mutex_unlock(&active_action->mutex);

        sleeptime_str = json_get_string(root, "$.config.polling.sleep", &error);
        if (!sleeptime_str) {
                g_warning("Polling sleep time not found: %s. Using fallback: %ds",
                          error->message, hawkbit_config->retry_wait);
                return hawkbit_config->retry_wait;
        }

        strptime(sleeptime_str, "%T", &time);
        return (time.tm_sec + (time.tm_min * 60) + (time.tm_hour * 60 * 60));
}

/**
 * @brief Build API URL
 *
 * @param path[in] a printf()-like format string describing the API path or NULL for base path
 * @param ... The arguments to be inserted in path
 *
 * @return a newly allocated full API URL
 */
__attribute__((__format__(__printf__, 1, 2)))
static gchar* build_api_url(const gchar *path, ...)
{
        g_autofree gchar *buffer = NULL;
        va_list args;

        if (path) {
                va_start(args, path);
                buffer = g_strdup_vprintf(path, args);
                va_end(args);
        }

        return g_strdup_printf(
                "%s://%s/%s/controller/v1/%s%s%s",
                hawkbit_config->ssl ? "https" : "http",
                hawkbit_config->hawkbit_server, hawkbit_config->tenant_id,
                hawkbit_config->controller_id,
                buffer ? "/" : "",
                buffer ? buffer : "");
}

gboolean hawkbit_progress(const gchar *msg)
{
        g_autofree gchar *feedback_url = NULL;
        g_autoptr(GError) error = NULL;

        g_return_val_if_fail(msg, FALSE);

        g_mutex_lock(&active_action->mutex);

        feedback_url = build_api_url("deploymentBase/%s/feedback", active_action->id);

        if (!feedback_progress(feedback_url, active_action->id, msg, &error))
                g_warning("%s", error->message);

        g_mutex_unlock(&active_action->mutex);

        return G_SOURCE_REMOVE;
}

/**
 * @brief Provide meta information that will allow the hawkBit to identify the device on a hardware
 * level.
 *
 * @see https://eclipse.dev/hawkbit/rest-api/rootcontroller-api-guide.html#_put_tenantcontrollerv1controlleridconfigdata
 *
 * @param[out] error Error
 * @return TRUE if identification succeeded, FALSE otherwise (error set)
 */
static gboolean identify(GError **error)
{
        g_autofree gchar *put_config_data_url = NULL;
        g_autoptr(JsonBuilder) builder = NULL;

        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        g_debug("Providing meta information to hawkbit server");
        put_config_data_url = build_api_url("configData");

        builder = json_build_status(NULL, NULL, "success", "closed", hawkbit_config->device);

        return rest_request_retriable(PUT, put_config_data_url, builder, NULL, error);
}

/**
 * @brief Deletes RAUC bundle at config's bundle_download_location (if given).
 */
static void process_deployment_cleanup()
{
        if (!hawkbit_config->bundle_download_location)
                return;

        if (!g_file_test(hawkbit_config->bundle_download_location, G_FILE_TEST_IS_REGULAR))
                return;

        if (g_remove(hawkbit_config->bundle_download_location))
                g_warning("Failed to delete file: %s", hawkbit_config->bundle_download_location);
}

gboolean install_complete_cb(gpointer ptr)
{
        gboolean res = FALSE;
        g_autoptr(GError) error = NULL;
        struct on_install_complete_userdata *result = ptr;
        g_autofree gchar *feedback_url = NULL;

        g_return_val_if_fail(ptr, FALSE);

        g_mutex_lock(&active_action->mutex);

        active_action->state = result->install_success ? ACTION_STATE_SUCCESS : ACTION_STATE_ERROR;
        feedback_url = build_api_url("deploymentBase/%s/feedback", active_action->id);
        res = feedback(
                feedback_url, active_action->id,
                result->install_success ? "Software bundle installed successfully."
                : "Failed to install software bundle.",
                result->install_success ? "success" : "failure",
                "closed", &error);

        if (!res)
                g_warning("%s", error->message);

        process_deployment_cleanup();
        g_mutex_unlock(&active_action->mutex);

        if (result->install_success && hawkbit_config->post_update_reboot) {
                sync();
                if (reboot(RB_AUTOBOOT) < 0)
                        g_critical("Failed to reboot: %s", g_strerror(errno));
        }

        return G_SOURCE_REMOVE;
}

/**
 * @brief Thread to download given Artifact, verfiy its checksum, send hawkBit
 * feedback and call software_ready_cb() callback on success.
 *
 * @param[in] data Artifact* to process
 * @return gpointer being 1 (TRUE) if download succeeded, 0 (FALSE) otherwise. The return value is
 *         meant to be used with the GPOINTER_TO_INT() macro only.
 *         Note that if the download thread waited for installation to finish ('run_once' mode),
 *         TRUE means both installation and download succeeded.
 */
static gpointer download_thread(gpointer data)
{
        struct on_new_software_userdata userdata = {
                .install_progress_callback = (GSourceFunc) hawkbit_progress,
                .install_complete_callback = install_complete_cb,
                .file = hawkbit_config->bundle_download_location,
                .auth_header = NULL,
                .ssl_verify = hawkbit_config->ssl_verify,
                .install_success = FALSE,
        };
        g_autoptr(GError) error = NULL, feedback_error = NULL;
        g_autofree gchar *msg = NULL;
        g_autofree gchar *calculated_sha1sum = NULL;
        g_autoptr(Artifact) artifact = data;
        curl_off_t speed;

        g_return_val_if_fail(data, NULL);

        g_assert_nonnull(hawkbit_config->bundle_download_location);
        g_debug("Attempting to download/resume file: %s", hawkbit_config->bundle_download_location);

        g_mutex_lock(&active_action->mutex);
        if (active_action->state == ACTION_STATE_CANCEL_REQUESTED)
                goto cancel;

        active_action->state = ACTION_STATE_DOWNLOADING;
        g_mutex_unlock(&active_action->mutex);

        g_message("Start downloading: %s", artifact->download_url);

#ifdef WITH_SYSTEMD
        sd_notify(0, "READY=1\nUPDATE=DOWNLOADING\nSTATUS=Installation running");
#endif

        while (1) {
                gboolean resumable = FALSE;
                GStatBuf bundle_stat;
                curl_off_t resume_from = 0;

                g_clear_pointer(&calculated_sha1sum, g_free);

                // Download software bundle (artifact)
                if (g_stat(hawkbit_config->bundle_download_location, &bundle_stat) == 0) {
                        if (bundle_stat.st_size > 0) {
                                resume_from = (curl_off_t) bundle_stat.st_size;
                                g_debug("Existing partial download found. Size: %" CURL_FORMAT_CURL_OFF_T " bytes", resume_from);
                        } else {
                                g_debug("Empty file found at download location. Starting new download.");
                                resume_from = 0;
                        }
                } else {
                        g_debug("No existing file found at download location. Starting new download.");
                        resume_from = 0;
                }
                if (get_binary(artifact->download_url, hawkbit_config->bundle_download_location,
                               resume_from, artifact->sha1, &calculated_sha1sum, &speed,
                               artifact->feedback_url, active_action->id, &error))
                        break;

                for (const gint *code = &resumable_codes[0]; *code; code++)
                        resumable |= g_error_matches(error, RHU_HAWKBIT_CLIENT_CURL_ERROR, *code);

                if (!hawkbit_config->resume_downloads || !resumable) {
                        g_prefix_error(&error, "Download failed: ");
                        goto report_err;
                }
                g_debug("%s, resuming download..", curl_easy_strerror(error->code));

                g_mutex_lock(&active_action->mutex);
                if (active_action->state == ACTION_STATE_CANCEL_REQUESTED)
                        goto cancel;
                g_mutex_unlock(&active_action->mutex);

                g_clear_error(&error);

                // sleep 0.5 s before attempting to resume download
                g_usleep(500000);
        }

        // notify hawkbit that download is complete
        msg = g_strdup_printf("Download complete. %.2f MB/s",
                              (double)speed/(1024*1024));
        g_mutex_lock(&active_action->mutex);
        if (!feedback_progress(artifact->feedback_url, active_action->id, msg, &error)) {
                g_warning("%s", error->message);
                g_clear_error(&error);
        }
        g_mutex_unlock(&active_action->mutex);

#ifdef WITH_SYSTEMD
        sd_notify(0, "READY=1\nUPDATE=DOWNLOADED\nSTATUS=Installation running");
#endif

        // validate checksum
        if (g_strcmp0(artifact->sha1, calculated_sha1sum)) {
                g_set_error(&error, RHU_HAWKBIT_CLIENT_ERROR, RHU_HAWKBIT_CLIENT_ERROR_DOWNLOAD,
                            "Software: %s V%s. Invalid checksum: %s expected %s", artifact->name,
                            artifact->version, calculated_sha1sum, artifact->sha1);
                goto report_err;
        }

        g_mutex_lock(&active_action->mutex);
        // skip installation if hawkBit asked us to do so
        if (!artifact->do_install &&
            (!artifact->maintenance_window || g_strcmp0(artifact->maintenance_window, "available") == 0)) {
                active_action->state = ACTION_STATE_SUCCESS;
                if (!feedback(artifact->feedback_url, active_action->id, "File checksum OK.", "success",
                              "downloaded", &feedback_error))
                        g_warning("%s", feedback_error->message);

                g_mutex_unlock(&active_action->mutex);
                return GINT_TO_POINTER(TRUE);
        }
        if (!feedback_progress(artifact->feedback_url, active_action->id, "File checksum OK.",
                               &error)) {
                g_warning("%s", error->message);
                g_clear_error(&error);
        }
        g_mutex_unlock(&active_action->mutex);

        // last chance to cancel installation

        g_mutex_lock(&active_action->mutex);
        if (active_action->state == ACTION_STATE_CANCEL_REQUESTED)
                goto cancel;

        // skip installation if hawkBit asked us to do so
        if (!artifact->do_install) {
                active_action->state = ACTION_STATE_NONE;
                g_mutex_unlock(&active_action->mutex);

                return GINT_TO_POINTER(TRUE);
        }

        // start installation, cancelations are impossible now
        active_action->state = ACTION_STATE_INSTALLING;
        g_cond_signal(&active_action->cond);
        g_mutex_unlock(&active_action->mutex);

        software_ready_cb(&userdata);

        return GINT_TO_POINTER(userdata.install_success);

report_err:
        g_mutex_lock(&active_action->mutex);
        if (!feedback(artifact->feedback_url, active_action->id, error->message, "failure",
                      "closed", &feedback_error))
                g_warning("%s", feedback_error->message);

        active_action->state = ACTION_STATE_ERROR;

        if (dbus_interface) {
                download_progress_download_progress_emit_error(dbus_interface, "EDOWNLOAD", error->message);
        }

cancel:
        if (active_action->state == ACTION_STATE_CANCEL_REQUESTED)
                active_action->state = ACTION_STATE_CANCELED;

        //process_deployment_cleanup();

        g_cond_signal(&active_action->cond);
        g_mutex_unlock(&active_action->mutex);

        return GINT_TO_POINTER(FALSE);
}

/**
 * @brief Start a RAUC HTTP streaming installation without prior bundle download.
 *        This skips the download thread and starts the install thread (via rauc_install()
 *        directly).
 *        Must be called under locked active_action->mutex.
 *
 * @param[in]  artifcat Artifact* to install
 * @param[out] error    Error
 * @return TRUE if installation prcessing succeeded, FALSE otherwise (error set).
 *         Note that this functions returns TRUE even for canceled/skipped installations.
 */
static gboolean start_streaming_installation(Artifact *artifact, GError **error)
{
        g_autofree gchar *auth_header = get_auth_header();
        struct on_new_software_userdata userdata = {
                .install_progress_callback = (GSourceFunc) hawkbit_progress,
                .install_complete_callback = install_complete_cb,
                .file = artifact->download_url,
                .auth_header = auth_header,
                .ssl_verify = hawkbit_config->ssl_verify,
                .install_success = FALSE,
        };

        // installation might already be canceled
        if (active_action->state == ACTION_STATE_CANCEL_REQUESTED) {
                active_action->state = ACTION_STATE_CANCELED;
                g_cond_signal(&active_action->cond);
                return TRUE;
        }

        // skip installation if hawkBit asked us to do so
        if (!artifact->do_install) {
                active_action->state = ACTION_STATE_NONE;
                return TRUE;
        }

        active_action->state = ACTION_STATE_INSTALLING;
        g_cond_signal(&active_action->cond);
        g_mutex_unlock(&active_action->mutex);

        software_ready_cb(&userdata);

        g_mutex_lock(&active_action->mutex);

        // in case of run_once, userdata.install_access is set and must be passed on
        if (!userdata.install_success) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_STREAM_INSTALL,
                            "Streaming installation failed");
                return FALSE;
        }

        return TRUE;
}

/**
 * @brief Process hawkBit deployment described by req_root.
 *        Must be called under locked active_action->mutex.
 *
 * @param[in]  req_root JsonNode* describing the deployment to process
 * @param[out] error    Error
 * @return TRUE if processing deployment succeeded, FALSE otherwise (error set)
 */
static gboolean process_deployment(JsonNode *req_root, GError **error)
{
        g_autoptr(Artifact) artifact = g_new0(Artifact, 1);
        g_autofree gchar *deployment = NULL, *temp_id = NULL,
                         *deployment_download = NULL, *deployment_update = NULL,
                         *maintenance_window = NULL, *maintenance_msg = NULL;
        g_autoptr(JsonParser) json_response_parser = NULL;
        g_autoptr(JsonArray) json_chunks = NULL, json_artifacts = NULL;
        JsonNode *resp_root = NULL, *json_chunk = NULL, *json_artifact = NULL;
        goffset freespace;

        g_return_val_if_fail(req_root, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        if (active_action->state >= ACTION_STATE_PROCESSING) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                            RHU_HAWKBIT_CLIENT_ERROR_ALREADY_IN_PROGRESS,
                            "Deployment %s is already in progress.", active_action->id);
                // no need to tell hawkBit about this
                return FALSE;
        }

        active_action->state = ACTION_STATE_PROCESSING;

        // get deployment URL
        deployment = json_get_string(req_root, "$._links.deploymentBase.href", error);
        if (!deployment)
                goto error;

        // get deployment resource
        if (!rest_request(GET, deployment, NULL, &json_response_parser, error))
                goto error;

        resp_root = json_parser_get_root(json_response_parser);

        // handle deployment.maintenanceWindow (only available if maintenance window is defined)
        maintenance_window = json_get_string(resp_root, "$.deployment.maintenanceWindow", NULL);
        artifact->maintenance_window = g_strdup(maintenance_window);
        maintenance_msg = maintenance_window
                          ? g_strdup_printf(" (maintenance window is '%s')", maintenance_window)
                          : g_strdup("");

        // handle deployment.download=skip
        deployment_download = json_get_string(resp_root, "$.deployment.download", error);
        if (!deployment_download)
                goto error;

        if (!g_strcmp0(deployment_download, "skip")) {
                g_message("hawkBit requested to skip download, not downloading yet%s.",
                          maintenance_msg);
                active_action->state = ACTION_STATE_NONE;
                return TRUE;
        }

        // handle deployment.update=skip
        deployment_update = json_get_string(resp_root, "$.deployment.update", error);
        if (!deployment_update)
                goto error;

        artifact->do_install = g_strcmp0(deployment_update, "skip") != 0;
        if (!artifact->do_install)
                g_message("hawkBit requested to skip installation, not invoking RAUC yet%s.",
                          maintenance_msg);

        // remember deployment's action id
        temp_id = json_get_string(resp_root, "$.id", error);

        if (!artifact->do_install && !g_strcmp0(temp_id, active_action->id)) {
                g_debug("Deployment %s is still waiting%s.", active_action->id, maintenance_msg);
                active_action->state = ACTION_STATE_NONE;
                return TRUE;
        }

        // clean up on changed deployment id
        if (g_strcmp0(temp_id, active_action->id))
                g_debug("Changed deployment id");
                //process_deployment_cleanup();
        else
                g_debug("Continuing scheduled deployment %s%s.", active_action->id,
                        maintenance_msg);

        g_free(active_action->id);
        active_action->id = g_steal_pointer(&temp_id);
        if (!active_action->id)
                goto error;

        artifact->feedback_url = build_api_url("deploymentBase/%s/feedback", active_action->id);

        // downloading multiple chunks not supported, only first chunk is downloaded (RAUC bundle)
        json_chunks = json_get_array(resp_root, "$.deployment.chunks", error);
        if (!json_chunks)
                goto proc_error;
        if (json_array_get_length(json_chunks) > 1) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR, RHU_HAWKBIT_CLIENT_ERROR_MULTI_CHUNKS,
                            "Deployment %s unsupported: cannot handle multiple chunks.", active_action->id);
                goto proc_error;
        }

        json_chunk = json_array_get_element(json_chunks, 0);

        // downloading multiple artifacts not supported, only first artifact is downloaded (RAUC bundle)
        json_artifacts = json_get_array(json_chunk, "$.artifacts", error);
        if (!json_artifacts)
                goto proc_error;
        if (json_array_get_length(json_artifacts) > 1) {
                g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR, RHU_HAWKBIT_CLIENT_ERROR_MULTI_ARTIFACTS,
                            "Deployment %s unsupported: cannot handle multiple artifacts.",
                            active_action->id);
                goto proc_error;
        }

        json_artifact = json_array_get_element(json_artifacts, 0);

        // get artifact information
        artifact->version = json_get_string(json_chunk, "$.version", error);
        if (!artifact->version)
                goto proc_error;

        artifact->name = json_get_string(json_chunk, "$.name", error);
        if (!artifact->name)
                goto proc_error;

        artifact->size = json_get_int(json_artifact, "$.size", error);
        if (!artifact->size && error)
                goto proc_error;

        artifact->sha1 = json_get_string(json_artifact, "$.hashes.sha1", error);
        if (!artifact->sha1)
                goto proc_error;

        // favour https download
        artifact->download_url = json_get_string(json_artifact, "$._links.download.href", NULL);
        if (!artifact->download_url)
                artifact->download_url = json_get_string(
                        json_artifact, "$._links.download-http.href", error);

        if (!artifact->download_url) {
                g_prefix_error(error, "\"$._links.download{-http,}.href\": ");
                goto proc_error;
        }

        g_message("New software ready for download (Name: %s, Version: %s, Size: %" G_GINT64_FORMAT " bytes, URL: %s)",
                  artifact->name, artifact->version, artifact->size, artifact->download_url);

#ifdef WITH_SYSTEMD
        sd_notify(0, "READY=1\nUPDATE=READY\nSTATUS=Installation running");
#endif

        // stream_bundle path exits early
        if (hawkbit_config->stream_bundle)
                return start_streaming_installation(artifact, error);


        // Check if a download is already in progress
        if (thread_download) {
                g_debug("Download thread already exists.");
                if (active_action->state == ACTION_STATE_DOWNLOADING) {
                        g_debug("Download is still in progress. Not starting a new one.");
                        return TRUE;
                } else {
                        g_debug("Previous download thread has finished. Cleaning up.");
                        g_thread_join(thread_download);
                        thread_download = NULL;
                }
        }

        // check if there is enough free diskspace
        if (!get_available_space(hawkbit_config->bundle_download_location, &freespace, error))
                goto proc_error;

        // Check if the file already exists and is complete
        unsigned long int partial_download_size = 0;
        {
                GStatBuf file_stat;
                if (g_stat(hawkbit_config->bundle_download_location, &file_stat) == 0) {
                        if (file_stat.st_size == artifact->size) {
                                struct on_new_software_userdata userdata = {
                                        .install_progress_callback = (GSourceFunc) hawkbit_progress,
                                        .install_complete_callback = install_complete_cb,
                                        .file = hawkbit_config->bundle_download_location,
                                        .auth_header = NULL,
                                        .ssl_verify = hawkbit_config->ssl_verify,
                                        .install_success = FALSE,
                                };

                                FILE *fp = NULL;
                                g_autofree gchar *calculated_sha1sum = NULL;
                                gchar **calculated_sha = NULL;
                                gsize file_size = 0;
                                g_debug("File already fully downloaded. Skipping download.");
                                g_clear_pointer(&calculated_sha1sum, g_free);
                                calculated_sha = &calculated_sha1sum;
                                g_return_val_if_fail(calculated_sha && *calculated_sha == NULL, FALSE);


                                // Reopen the file for checksum calculation
                                fp = g_fopen(userdata.file, "rb");
                                if (!fp) {
                                        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                        "Failed to open %s for checksum calculation: %s", userdata.file, g_strerror(errno));
                                        return FALSE;
                                }

                                // Get the file size
                                fseek(fp, 0, SEEK_END);
                                file_size = ftell(fp);
                                fseek(fp, 0, SEEK_SET);

                                g_debug("Calculating checksum for entire file. Size: %zu bytes", file_size);

                                // Calculate checksum for the entire file
                                if (!get_file_checksum(fp, G_CHECKSUM_SHA1, calculated_sha, file_size, error)) {
                                        fclose(fp);
                                        process_deployment_cleanup();
                                        return FALSE;
                                }

                                fclose(fp);

                                if (artifact->sha1 && g_strcmp0(*calculated_sha, artifact->sha1) != 0) {
                                        g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR, RHU_HAWKBIT_CLIENT_ERROR_CHECKSUM,
                                                "Checksum mismatch. Expected: %s, Calculated: %s",
                                                artifact->sha1, *calculated_sha);
                                        process_deployment_cleanup();
                                        return FALSE;
                                }
                                g_debug("Checksum valid, installing");

                                active_action->state = ACTION_STATE_INSTALLING;
                                g_cond_signal(&active_action->cond);
                                g_mutex_unlock(&active_action->mutex);

                                software_ready_cb(&userdata);

                                g_mutex_lock(&active_action->mutex);

                                if (!userdata.install_success) {
                                        g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                                                RHU_HAWKBIT_CLIENT_ERROR_STREAM_INSTALL,
                                                "Installation failed");
                                        process_deployment_cleanup();
                                        return FALSE;
                                }

                                return TRUE;
                        } else if (file_stat.st_size > 0) {
                                partial_download_size = file_stat.st_size;
                                g_debug("Partial download found. Will resume from byte %" G_GOFFSET_FORMAT, 
                                        (goffset)file_stat.st_size);
                        }
                }
        }

        if (freespace + partial_download_size < artifact->size) { // take into account partial download size
                // notify hawkbit that there is not enough free space
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSPC,
                            "File size %" G_GINT64_FORMAT " exceeds available space %" G_GOFFSET_FORMAT,
                            artifact->size, freespace);
                goto proc_error;
        }

        // Start new download thread
        g_debug("Starting new download thread");
        active_action->state = ACTION_STATE_DOWNLOADING;
        thread_download = g_thread_new("downloader", download_thread,
                                       (gpointer) g_steal_pointer(&artifact));

        return TRUE;

proc_error:
        feedback(artifact->feedback_url, active_action->id, (*error)->message, "failure", "closed", NULL);

        if (dbus_interface) {
                download_progress_download_progress_emit_error(dbus_interface, "EPRODEP" ,(*error)->message);
        }

error:
        // clean up failed deployment
        //process_deployment_cleanup();
        active_action->state = ACTION_STATE_NONE;
        return FALSE;
}

/**
 * @brief Process hawkBit cancel action described by req_root.
 *
 * @param[in]  req_root JsonNode* describing the cancel action
 * @param[out] error    Error
 * @return TRUE if cancel action succeeded, FALSE otherwise (error set)
 */
static gboolean process_cancel(JsonNode *req_root, GError **error)
{
        gboolean res = TRUE;
        g_autofree gchar *cancel_url = NULL, *feedback_url = NULL, *stop_id = NULL, *msg = NULL;
        g_autoptr(JsonParser) json_response_parser = NULL;
        JsonNode *resp_root = NULL;

        g_return_val_if_fail(req_root, FALSE);
        g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

        // get cancel url
        cancel_url = json_get_string(req_root, "$._links.cancelAction.href", error);
        if (!cancel_url)
                return FALSE;

        // retrieve cancel details
        if (!rest_request(GET, cancel_url, NULL, &json_response_parser, error))
                return FALSE;

        resp_root = json_parser_get_root(json_response_parser);

        // retrieve stop id
        stop_id = json_get_string(resp_root, "$.cancelAction.stopId", error);
        if (!stop_id)
                return FALSE;

        g_message("Received cancelation for action %s", stop_id);

        // send cancel feedback
        feedback_url = build_api_url("cancelAction/%s/feedback", stop_id);

        // cancel action if install not started yet
        g_mutex_lock(&active_action->mutex);
        if (!g_strcmp0(stop_id, active_action->id) &&
            (active_action->state == ACTION_STATE_PROCESSING ||
             active_action->state == ACTION_STATE_DOWNLOADING)) {

                g_debug("Action %s is in state %d, waiting for cancel request to be processed",
                        stop_id, active_action->state);
                active_action->state = ACTION_STATE_CANCEL_REQUESTED;

                while (active_action->state == ACTION_STATE_CANCEL_REQUESTED)
                        g_cond_wait(&active_action->cond, &active_action->mutex);
        }
        if (g_strcmp0(stop_id, active_action->id))
                active_action->state = ACTION_STATE_NONE;

        // send feedback
        switch (active_action->state) {
        case ACTION_STATE_NONE:
                // action unknown, acknowledge cancelation nonetheless
                g_debug("Received cancelation for unprocessed action %s, acknowledging.",
                        stop_id);
        // fall through
        case ACTION_STATE_CANCELED:
                res = feedback(feedback_url, stop_id, "Action canceled.", "success", "closed",
                               error);
                if (res) {
                        process_deployment_cleanup();
                }
                break;
        case ACTION_STATE_SUCCESS:
                g_debug("Cancelation impossible, installation succeeded already");
                break;
        case ACTION_STATE_ERROR:
                g_debug("Cancelation impossible, installation failed already");
                break;
        case ACTION_STATE_INSTALLING:
                msg = g_strdup("Cancelation impossible, installation started already.");
                res = feedback(feedback_url, stop_id, msg, "success", "rejected", error);
                if (res) {
                        res = FALSE;
                        g_set_error(error, RHU_HAWKBIT_CLIENT_ERROR,
                                    RHU_HAWKBIT_CLIENT_ERROR_CANCELATION, "%s", msg);
                }
                break;
        default:
                // other states are not expected here
                g_critical("Unexpected action state after cancel request: %d", active_action->state);
                g_assert_not_reached();
                break;
        }

        g_mutex_unlock(&active_action->mutex);
        return res;
}

void hawkbit_init(Config *config, GSourceFunc on_install_ready)
{
        GError *error = NULL;
        GDBusConnection *connection = NULL;
        g_return_if_fail(config);

        hawkbit_config = config;
        software_ready_cb = on_install_ready;
        curl_global_init(CURL_GLOBAL_ALL);

        init_dbus_service();

        connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (connection == NULL) {
                g_print("Failed to connect to D-Bus: %s\n", error->message);
                g_error_free(error);
        } else {
                on_bus_acquired(connection, "org.hawkbit.DownloadProgress", NULL);
                g_object_unref(connection);
        }
}

typedef struct ClientData_ {
        GMainLoop *loop;
        gboolean res;
        long hawkbit_interval_check_sec;
        long last_run_sec;
} ClientData;

/**
 * @brief Callback for main loop, should run regularly, polls controller base poll resource and
 * triggers appropriate actions.
 *
 * @param[in] user_data ClientData*
 * @return TRUE if polling controller base resource and running appropriate actions succeeded,
 *         FALSE otherwise
 */
static gboolean hawkbit_pull_cb(gpointer user_data)
{
        ClientData *data = user_data;
        gboolean res = FALSE;
        g_autoptr(GError) error = NULL;
        g_autofree gchar *get_tasks_url = NULL;
        g_autoptr(JsonParser) json_response_parser = NULL;
        JsonNode *json_root = NULL;

        g_return_val_if_fail(user_data, FALSE);

        if (++data->last_run_sec < data->hawkbit_interval_check_sec)
                return G_SOURCE_CONTINUE;

        data->last_run_sec = 0;

        // build hawkBit get tasks URL
        get_tasks_url = build_api_url(NULL);

        g_message("Checking for new software...");
        res = rest_request(GET, get_tasks_url, NULL, &json_response_parser, &error);
        if (!res) {
                if (g_error_matches(error, RHU_HAWKBIT_CLIENT_HTTP_ERROR, 401)) {
                        if (hawkbit_config->auth_token)
                                g_warning("Failed to authenticate. Check if auth_token is correct?");
                        if (hawkbit_config->gateway_token)
                                g_warning("Failed to authenticate. Check if gateway_token is correct?");
                } else {
                        g_warning("Scheduled check for new software failed: %s (%d)",
                                  error->message, error->code);
                }

                data->hawkbit_interval_check_sec = hawkbit_config->retry_wait;
                goto out;
        }

        // owned by the JsonParser and should never be modified or freed
        json_root = json_parser_get_root(json_response_parser);

        if (json_contains(json_root, "$._links.configData")) {
                // hawkBit has asked us to identify ourselves
                res = identify(&error);
                if (!res) {
                        g_warning("%s", error->message);
                        g_clear_error(&error);
                }
        }
        if (json_contains(json_root, "$._links.deploymentBase")) {
                // hawkBit has a new deployment for us
                g_mutex_lock(&active_action->mutex);
                res = process_deployment(json_root, &error);
                g_mutex_unlock(&active_action->mutex);
                if (!res) {
                        if (g_error_matches(error, RHU_HAWKBIT_CLIENT_ERROR,
                                            RHU_HAWKBIT_CLIENT_ERROR_ALREADY_IN_PROGRESS))
                                g_debug("%s", error->message);
                        else
                                g_warning("%s", error->message);
                }
        } else {
                g_message("No new software.");
        }
        if (json_contains(json_root, "$._links.cancelAction")) {
                res = process_cancel(json_root, &error);
                if (!res) {
                        g_warning("%s", error->message);
                        g_clear_error(&error);
                }
        }

        // get hawkbit sleep time (how often should we check for new software)
        data->hawkbit_interval_check_sec = json_get_sleeptime(json_root);

out:
        if (run_once) {
                if (thread_download) {
                        gpointer thread_ret = g_thread_join(thread_download);
                        res = GPOINTER_TO_INT(thread_ret);
                }

                data->res = res;
                g_main_loop_quit(data->loop);
                return G_SOURCE_REMOVE;
        }

        return G_SOURCE_CONTINUE;
}

int hawkbit_start_service_sync()
{
        g_autoptr(GMainContext) ctx = NULL;
        ClientData cdata;
        g_autoptr(GSource) timeout_source = NULL;
        int res = 0;
#ifdef WITH_SYSTEMD
        g_autoptr(GSource) event_source = NULL;
        g_autoptr(sd_event) event = NULL;
#endif

        active_action = action_new();

        ctx = g_main_context_new();
        cdata.loop = g_main_loop_new(ctx, FALSE);
        cdata.hawkbit_interval_check_sec = hawkbit_config->retry_wait;
        cdata.last_run_sec = hawkbit_config->retry_wait;

        // pull every second
        timeout_source = g_timeout_source_new(1000);
        g_source_set_name(timeout_source, "Add timeout");
        g_source_set_callback(timeout_source, (GSourceFunc) hawkbit_pull_cb, &cdata, NULL);
        g_source_attach(timeout_source, ctx);

#ifdef WITH_SYSTEMD
        res = sd_event_default(&event);
        if (res < 0)
                goto finish;
        // enable automatic service watchdog support
        res = sd_event_set_watchdog(event, TRUE);
        if (res < 0)
                goto finish;

        event_source = sd_source_new(event);
        if (!event_source) {
                res = -ENOMEM;
                goto finish;
        }

        // attach systemd source to glib mainloop
        res = sd_source_attach(event_source, cdata.loop);
        if (res < 0)
                goto finish;

        sd_notify(0, "READY=1\nSTATUS=Init completed, start polling HawkBit for new software.");
#endif

        g_main_loop_run(cdata.loop);

        res = cdata.res ? 0 : 1;

#ifdef WITH_SYSTEMD
        sd_notify(0, "STOPPING=1\nSTATUS=Stopped polling HawkBit for new software.");
#endif

#ifdef WITH_SYSTEMD
finish:
        g_source_destroy(event_source);
        sd_event_set_watchdog(event, FALSE);
#endif
        g_main_loop_unref(cdata.loop);
        if (res < 0)
                g_warning("%s", strerror(-res));

        return res;
}

void artifact_free(Artifact *artifact)
{
        if (!artifact)
                return;

        g_free(artifact->name);
        g_free(artifact->version);
        g_free(artifact->download_url);
        g_free(artifact->feedback_url);
        g_free(artifact->sha1);
        g_free(artifact->maintenance_window);
        g_free(artifact);
}

void rest_payload_free(RestPayload *payload)
{
        if (!payload)
                return;

        g_free(payload->payload);
        g_free(payload);
}
