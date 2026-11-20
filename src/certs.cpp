#include "certs.h"
#include "system.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QRegularExpression>
#include <QSslCertificate>

namespace {

const char *Nickname = "Loft CA";

// The base64 body of a PEM file, so two files compare regardless of line
// breaks and headers.
QByteArray pemBody(const QByteArray &pem) {
    QByteArray body;
    for (const QByteArray &line : pem.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith("-----"))
            body += trimmed;
    }
    return body;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool openssl(const QStringList &arguments, QString *error) {
    QString output;
    if (System::run("openssl", arguments, &output) == 0)
        return true;
    if (error)
        *error = output.trimmed().isEmpty() ? QStringLiteral("openssl failed") : output.trimmed().split('\n').last();
    return false;
}

} // namespace

namespace Certs {

QString caCert(const Paths &paths) { return paths.certs() + "/ca.crt"; }
QString caKey(const Paths &paths) { return paths.certs() + "/ca.key"; }
QString siteCert(const Paths &paths, const QString &host) { return paths.certs() + "/" + host + ".crt"; }
QString siteKey(const Paths &paths, const QString &host) { return paths.certs() + "/" + host + ".key"; }

bool ensureCa(const Paths &paths, QString *error) {
    if (QFileInfo::exists(caCert(paths)) && QFileInfo::exists(caKey(paths)))
        return true;
    QDir().mkpath(paths.certs());
    QFile::setPermissions(paths.certs(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString who = qEnvironmentVariable("USER", "user") + "@" + QHostInfo::localHostName();
    const bool ok = openssl({"req", "-x509", "-new", "-nodes",
                             "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                             "-keyout", caKey(paths), "-out", caCert(paths), "-days", "3650",
                             "-subj", "/O=Loft/CN=Loft CA (" + who + ")",
                             "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
                             "-addext", "keyUsage=critical,keyCertSign,cRLSign"},
                            error);
    if (ok)
        QFile::setPermissions(caKey(paths), QFile::ReadOwner | QFile::WriteOwner);
    return ok;
}

bool siteValid(const Paths &paths, const QString &host) {
    const auto certs = QSslCertificate::fromPath(siteCert(paths, host));
    const auto cas = QSslCertificate::fromPath(caCert(paths));
    if (certs.isEmpty() || cas.isEmpty() || !QFileInfo::exists(siteKey(paths, host)))
        return false;
    const QSslCertificate &cert = certs.first(), &ca = cas.first();
    if (cert.expiryDate() < QDateTime::currentDateTimeUtc().addDays(30))
        return false;
    if (!cert.subjectAlternativeNames().values(QSsl::DnsEntry).contains(host))
        return false;
    // A CA made after this certificate means the old CA is gone, and with it
    // the signature browsers would check.
    return cert.issuerDisplayName() == ca.subjectDisplayName() && cert.effectiveDate() >= ca.effectiveDate();
}

bool ensureSite(const Paths &paths, const QString &host, QString *error) {
    if (!ensureCa(paths, error))
        return false;
    if (siteValid(paths, host))
        return true;
    const bool ok = openssl({"req", "-x509", "-new", "-nodes",
                             "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                             "-keyout", siteKey(paths, host), "-out", siteCert(paths, host),
                             "-days", "397", "-subj", "/O=Loft/CN=" + host,
                             "-CA", caCert(paths), "-CAkey", caKey(paths),
                             "-addext", "subjectAltName=DNS:" + host + ",DNS:*." + host,
                             "-addext", "basicConstraints=critical,CA:FALSE",
                             "-addext", "keyUsage=critical,digitalSignature,keyEncipherment",
                             "-addext", "extendedKeyUsage=serverAuth"},
                            error);
    if (ok)
        QFile::setPermissions(siteKey(paths, host), QFile::ReadOwner | QFile::WriteOwner);
    return ok;
}

void removeSite(const Paths &paths, const QString &host) {
    QFile::remove(siteCert(paths, host));
    QFile::remove(siteKey(paths, host));
}

bool trustedBySystem(const Paths &paths, const QString &bundle) {
    const QByteArray ca = pemBody(readFile(caCert(paths)));
    return !ca.isEmpty() && pemBody(readFile(bundle)).contains(ca);
}

QStringList nssDatabases(const Paths &paths) {
    QStringList databases{paths.home + "/.pki/nssdb"};
    for (const QString &root : {paths.home + "/.mozilla/firefox", paths.home + "/.config/mozilla/firefox"}) {
        const auto profiles = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &profile : profiles)
            if (QFileInfo::exists(profile.filePath() + "/cert9.db"))
                databases << profile.absoluteFilePath();
    }
    return databases;
}

static bool inDatabase(const QString &database, const QByteArray &ca) {
    QString output;
    if (System::run("certutil", {"-d", "sql:" + database, "-L", "-n", Nickname, "-a"}, &output) != 0)
        return false;
    return pemBody(output.toUtf8()) == ca;
}

bool trustedByBrowsers(const Paths &paths) {
    const QByteArray ca = pemBody(readFile(caCert(paths)));
    if (ca.isEmpty())
        return false;
    for (const QString &database : nssDatabases(paths))
        if (!inDatabase(database, ca))
            return false;
    return true;
}

bool trustInBrowsers(const Paths &paths, QString *error) {
    if (System::which("certutil").isEmpty()) {
        if (error)
            *error = QStringLiteral("certutil is missing (pacman -S nss)");
        return false;
    }
    const QByteArray ca = pemBody(readFile(caCert(paths)));
    for (const QString &database : nssDatabases(paths)) {
        if (inDatabase(database, ca))
            continue;
        if (!QFileInfo::exists(database + "/cert9.db")) {
            QDir().mkpath(database);
            System::run("certutil", {"-d", "sql:" + database, "-N", "--empty-password"});
        }
        // A CA left from an earlier setup has the same nickname; replace it.
        System::run("certutil", {"-d", "sql:" + database, "-D", "-n", Nickname});
        QString output;
        if (System::run("certutil", {"-d", "sql:" + database, "-A", "-t", "C,,", "-n", Nickname,
                                     "-i", caCert(paths)}, &output) != 0) {
            if (error)
                *error = output.trimmed();
            return false;
        }
    }
    return true;
}

void untrustInBrowsers(const Paths &paths) {
    for (const QString &database : nssDatabases(paths))
        System::run("certutil", {"-d", "sql:" + database, "-D", "-n", Nickname});
}

} // namespace Certs
