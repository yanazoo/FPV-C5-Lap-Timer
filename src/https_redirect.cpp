// Kept apart from the AsyncWebServer code: esp_http_server's method names
// (HTTP_GET, ...) clash with ESPAsyncWebServer's.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <esp_random.h>
#include <esp_system.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include "https_redirect.h"

namespace {

// Set while the certificate is made and the server starts; survives a crash
// reset. If that ever crashes, the next boot skips HTTPS instead of crashing
// again, so the access point and the web UI always come up.
const uint32_t kHttpsTrying = 0x4854505A;
RTC_NOINIT_ATTR uint32_t rtcHttpsTrying;

httpd_handle_t https = nullptr;
String certPem, keyPem;

int rng(void*, unsigned char* buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// A P-256 key and a self-signed certificate for it, as PEM.
bool makeCertificate(String& cert, String& key) {
    bool ok = false;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    static unsigned char buf[1600];
    do {
        if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
        if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, nullptr) != 0) break;
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=fpv-c5-lt.local,O=FPV C5 Lap Timer") != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=fpv-c5-lt.local,O=FPV C5 Lap Timer") != 0) break;
        unsigned char serial[8];
        esp_fill_random(serial, sizeof serial);
        serial[0] &= 0x7F;   // positive
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial) != 0) break;
        if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959") != 0) break;
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
        if (mbedtls_x509write_crt_pem(&crt, buf, sizeof buf, rng, nullptr) != 0) break;
        cert = (const char*)buf;
        if (mbedtls_pk_write_key_pem(&pk, buf, sizeof buf) != 0) break;
        key = (const char*)buf;
        ok = true;
    } while (false);
    memset(buf, 0, sizeof buf);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    return ok;
}

bool loadOrMakeCertificate() {
    Preferences p;
    p.begin("tls", false);
    certPem = p.getString("crt", "");
    keyPem = p.getString("key", "");
    if (certPem.isEmpty() || keyPem.isEmpty()) {
        Serial.println("[HTTPS] making a self-signed certificate");
        if (!makeCertificate(certPem, keyPem)) {
            p.end();
            certPem = keyPem = "";
            return false;
        }
        p.putString("crt", certPem);
        p.putString("key", keyPem);
    }
    p.end();
    return true;
}

// Only a plain host name or address goes into the Location header.
bool safeHost(const char* h) {
    if (!*h) return false;
    for (const char* c = h; *c; ++c)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') return false;
    return true;
}

esp_err_t redirect(httpd_req_t* req) {
    char host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) == ESP_OK) {
        char* colon = strchr(host, ':');
        if (colon) *colon = '\0';   // drop :443
    }
    if (!safeHost(host)) {
        IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
        strlcpy(host, ip.toString().c_str(), sizeof host);
    }
    String to = String("http://") + host + (req->uri[0] == '/' ? req->uri : "/");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", to.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    String body = "<a href=\"" + to + "\">" + to + "</a>";
    return httpd_resp_send(req, body.c_str(), body.length());
}

}  // namespace

static void startServer() {
    if (!loadOrMakeCertificate()) {
        Serial.println("[HTTPS] no certificate, https:// redirect off");
        return;
    }
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.servercert = (const uint8_t*)certPem.c_str();
    conf.servercert_len = certPem.length() + 1;
    conf.prvtkey_pem = (const uint8_t*)keyPem.c_str();
    conf.prvtkey_len = keyPem.length() + 1;
    // Only ever a redirect: one connection, below the web UI's priority so a
    // TLS handshake never holds up the race display.
    conf.httpd.max_open_sockets = 1;
    conf.httpd.task_priority = 1;
    conf.httpd.lru_purge_enable = true;
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
    conf.httpd.ctrl_port = 32790;
    if (httpd_ssl_start(&https, &conf) != ESP_OK) {
        Serial.println("[HTTPS] server start failed");
        https = nullptr;
        return;
    }
    for (httpd_method_t m : {HTTP_GET, HTTP_HEAD, HTTP_POST}) {
        httpd_uri_t u = {};
        u.uri = "/*";
        u.method = m;
        u.handler = redirect;
        httpd_register_uri_handler(https, &u);
    }
    Serial.println("[HTTPS] port 443: redirecting to http://");
}

// Making the key and signing the certificate needs more stack than the
// Arduino loop task has (8 KB): run it in a task of its own, a moment after
// boot, so the access point and the web UI are already up.
static void httpsTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    rtcHttpsTrying = kHttpsTrying;
    startServer();
    rtcHttpsTrying = 0;
    vTaskDelete(nullptr);
}

void httpsRedirectBegin() {
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (crashed && rtcHttpsTrying == kHttpsTrying) {
        rtcHttpsTrying = 0;
        Serial.println("[HTTPS] crashed starting last time: https:// redirect off this boot");
        return;
    }
    rtcHttpsTrying = 0;
    xTaskCreatePinnedToCore(httpsTask, "https", 16384, nullptr, 1, nullptr, 1);
}
