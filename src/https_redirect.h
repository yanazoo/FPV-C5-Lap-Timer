// https:// -> http:// redirect.
//
// The web UI is plain HTTP on port 80. A browser opened at https://... (or
// one that upgrades a typed address to HTTPS) reaches port 443, where this
// small TLS server answers every request with a redirect to the same path
// over http://. The certificate is self-signed and made on the device at the
// first boot (kept in NVS), so no private key is in the repository; the
// browser shows a certificate warning once before it follows the redirect.
#pragma once

void httpsRedirectBegin();
