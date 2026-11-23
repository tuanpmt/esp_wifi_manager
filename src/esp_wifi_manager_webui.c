/**
 * @file esp_wifi_manager_webui.c
 * @brief Embedded Web UI serving for WiFi Manager
 */

#include "esp_wifi_manager_priv.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CONFIG_WIFI_MGR_ENABLE_WEBUI

static const char *TAG = "wifi_mgr_webui";

// Embedded files (linked via CMakeLists.txt EMBED_FILES)
// Symbol names: _binary_{filename with . replaced by _}_{start|end}
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t index_css_gz_start[] asm("_binary_index_css_gz_start");
extern const uint8_t index_css_gz_end[] asm("_binary_index_css_gz_end");

/**
 * @brief Get the filesystem base path for custom WebUI files
 *
 * Returns the CONFIG_WIFI_MGR_WEBUI_CUSTOM_PATH if set, NULL otherwise.
 */
static const char *get_fs_base_path(void)
{
#ifdef CONFIG_WIFI_MGR_WEBUI_CUSTOM_PATH
    const char *path = CONFIG_WIFI_MGR_WEBUI_CUSTOM_PATH;
    if (path && path[0]) {
        return path;
    }
#endif
    return NULL;
}

/**
 * @brief Try to serve a file from the custom filesystem path (SPIFFS/LittleFS)
 *
 * @param uri Request URI or fixed asset path; sanitized before use.
 *            A pre-compressed "<file>.gz" is preferred when present.
 */
static bool serve_from_filesystem(httpd_req_t *req, const char *uri)
{
    const char *base_path = get_fs_base_path();
    if (!base_path) {
        return false;
    }

    char relpath[96];
    if (!wifi_mgr_sm_sanitize_path(relpath, sizeof(relpath), uri)) {
        ESP_LOGW(TAG, "Rejected path: %s", uri);
        return false;
    }
    if (strcmp(relpath, "/") == 0) {
        strcpy(relpath, "/index.html");
    }

    // A request for "x.js.gz" itself is served verbatim as application/gzip;
    // otherwise a pre-compressed "<file>.gz" is preferred.
    size_t rel_len = strlen(relpath);
    bool direct_gz = rel_len > 3 && strcmp(relpath + rel_len - 3, ".gz") == 0;

    char fullpath[160];
    int n = snprintf(fullpath, sizeof(fullpath), "%s%s%s", base_path, relpath, direct_gz ? "" : ".gz");
    if (n < 0 || n >= (int)sizeof(fullpath)) {
        return false;
    }

    struct stat st;
    bool gzipped = !direct_gz && stat(fullpath, &st) == 0 && S_ISREG(st.st_mode);
    if (direct_gz) {
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            return false;
        }
    } else if (!gzipped) {
        fullpath[n - 3] = '\0';  // drop ".gz"
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            return false;
        }
    }

    FILE *f = fopen(fullpath, "r");
    if (!f) {
        return false;
    }

    httpd_resp_set_type(req, wifi_mgr_sm_mime_type(relpath));
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    char *buf = malloc(1024);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t read_bytes;
    esp_err_t ret = ESP_OK;
    while (ret == ESP_OK && (read_bytes = fread(buf, 1, 1024, f)) > 0) {
        ret = httpd_resp_send_chunk(req, buf, read_bytes);
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    free(buf);
    fclose(f);
    ESP_LOGD(TAG, "Served from filesystem: %s", fullpath);
    return true;
}

/**
 * @brief Handler for index.html (root path)
 */
static esp_err_t handler_webui_index(httpd_req_t *req)
{
    // Try filesystem first
    if (serve_from_filesystem(req, "/index.html")) {
        return ESP_OK;
    }

    // Serve embedded index.html
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

/**
 * @brief Handler for app.js
 */
static esp_err_t handler_webui_app_js(httpd_req_t *req)
{
    // Try filesystem first
    if (serve_from_filesystem(req, "/assets/app.js")) {
        return ESP_OK;
    }

    // Serve embedded gzipped file
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)app_js_gz_start,
                    app_js_gz_end - app_js_gz_start);
    return ESP_OK;
}

/**
 * @brief Handler for index.css
 */
static esp_err_t handler_webui_index_css(httpd_req_t *req)
{
    // Try filesystem first
    if (serve_from_filesystem(req, "/assets/index.css")) {
        return ESP_OK;
    }

    // Serve embedded gzipped file
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)index_css_gz_start,
                    index_css_gz_end - index_css_gz_start);
    return ESP_OK;
}

/**
 * @brief Wildcard handler for additional static files from filesystem
 *
 * Serves any file found on the configured filesystem path that doesn't
 * match the explicit handlers (index, app.js, index.css). Returns 404
 * if the file doesn't exist on the filesystem.
 */
static esp_err_t handler_webui_wildcard(httpd_req_t *req)
{
    // req->uri is the file path (e.g., "/assets/logo.png")
    if (serve_from_filesystem(req, req->uri)) {
        return ESP_OK;
    }

    // Unknown path: send captive clients to the portal, others get 404
    if (wifi_mgr_http_is_captive_client(req)) {
        return wifi_mgr_http_captive_redirect(req);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return ESP_FAIL;
}

/**
 * @brief Initialize Web UI handlers
 */
esp_err_t wifi_mgr_webui_init(httpd_handle_t httpd)
{
    if (!httpd) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *fs_path = get_fs_base_path();

    ESP_LOGI(TAG, "Initializing Web UI (fs_path: %s)", fs_path ? fs_path : "(embedded only)");

    // Register explicit handlers for known files
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_webui_index,
    };
    wifi_mgr_http_register(&index_uri);

    httpd_uri_t app_js_uri = {
        .uri = "/assets/app.js",
        .method = HTTP_GET,
        .handler = handler_webui_app_js,
    };
    wifi_mgr_http_register(&app_js_uri);

    httpd_uri_t css_uri = {
        .uri = "/assets/index.css",
        .method = HTTP_GET,
        .handler = handler_webui_index_css,
    };
    wifi_mgr_http_register(&css_uri);

    // Register wildcard handler for additional static files from filesystem
    if (fs_path) {
        httpd_uri_t wildcard_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = handler_webui_wildcard,
        };
        wifi_mgr_http_register(&wildcard_uri);
        ESP_LOGI(TAG, "Wildcard file serving enabled from %s", fs_path);
    }

    size_t total_size = (index_html_end - index_html_start) +
                        (app_js_gz_end - app_js_gz_start) +
                        (index_css_gz_end - index_css_gz_start);
    ESP_LOGI(TAG, "Web UI registered (embedded size: %zu bytes)", total_size);

    return ESP_OK;
}

#else

esp_err_t wifi_mgr_webui_init(httpd_handle_t httpd)
{
    (void)httpd;
    return ESP_OK;
}

#endif // CONFIG_WIFI_MGR_ENABLE_WEBUI
