#pragma once

#include "core.h"

#include <QString>
#include <QStringList>

// Loft's own certificate authority, and one certificate per secured site
// signed by it. Everything is made with the openssl command.
namespace Certs {

QString caCert(const Paths &paths);
QString caKey(const Paths &paths);
QString siteCert(const Paths &paths, const QString &host);
QString siteKey(const Paths &paths, const QString &host);

bool ensureCa(const Paths &paths, QString *error);

// Makes the certificate when it is missing, expires within 30 days, lacks
// the host, or predates the current CA. Returns false with a reason on error.
bool ensureSite(const Paths &paths, const QString &host, QString *error);
bool siteValid(const Paths &paths, const QString &host);
void removeSite(const Paths &paths, const QString &host);

// The CA as Arch's bundle and Chromium/Firefox see it.
bool trustedBySystem(const Paths &paths, const QString &bundle = "/etc/ssl/certs/ca-certificates.crt");
bool trustedByBrowsers(const Paths &paths);
bool trustInBrowsers(const Paths &paths, QString *error);
void untrustInBrowsers(const Paths &paths);
// Chromium's ~/.pki/nssdb and every Firefox profile.
QStringList nssDatabases(const Paths &paths);

} // namespace Certs
