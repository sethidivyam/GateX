// ─────────────────────────────────────────────────────────────────────────────
//  certs.h — AWS IoT Certificates
//  Download from AWS IoT Console when creating the Thing
//  Place the actual PEM content between the R"(   )" delimiters
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

// AWS Root CA (AmazonRootCA1.pem)
static const char AWS_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_YOUR_AWS_ROOT_CA_HERE
-----END CERTIFICATE-----
)EOF";

// Device Certificate (xxx-certificate.pem.crt)
static const char DEVICE_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_YOUR_DEVICE_CERTIFICATE_HERE
-----END CERTIFICATE-----
)EOF";

// Device Private Key (xxx-private.pem.key)
static const char DEVICE_PRIVATE_KEY[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PASTE_YOUR_PRIVATE_KEY_HERE
-----END RSA PRIVATE KEY-----
)EOF";
