#pragma once

#include "core.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QUrl>
#include <functional>

// A PHP minor line's newest static build for this machine, from static-php.dev.
struct PhpRelease {
    QString minor;      // "8.4"
    QString version;    // "8.4.27"
    QString cliFile, cliSha;
    QString fpmFile, fpmSha;
    qint64 cliSize = 0, fpmSize = 0;
};

// A Node major line's newest release, from nodejs.org.
struct NodeRelease {
    QString major;      // "24"
    QString version;    // "24.22.3"
    QString lts;        // codename ("Krypton"), empty for a current release
    QString date;       // "2026-11-10"
};

struct Catalog {
    QMap<QString, PhpRelease> php;   // by minor
    QList<NodeRelease> node;         // newest major first
    QDateTime fetched;

    QJsonObject toJson() const;
    static Catalog fromJson(const QJsonObject &object);
    const NodeRelease *nodeRelease(const QString &major) const;
};

struct Update {
    QString tool;        // "php" or "node"
    QString line;        // "8.4" or "24"
    QString installed;   // "8.4.26"
    QString latest;      // "8.4.27"
};

namespace Runtimes {

using Progress = std::function<void(qint64 received, qint64 total)>;

QString phpPlatform();    // linux-x86_64, linux-aarch64
QString nodePlatform();   // linux-x64, linux-arm64
QString phpBase(const QString &build);

// Catalog files, parsed without the network so tests can feed them.
QMap<QString, QString> parsePhpLatest(const QByteArray &versionsJson);
bool parsePhpRelease(const QByteArray &versionJson, const QString &platform, PhpRelease *release);
// The newest release of each line worth offering: LTS lines with a release in
// the last 18 months, plus the newest line even when it isn't LTS yet.
QList<NodeRelease> parseNodeIndex(const QByteArray &indexJson, const QDate &today = QDate::currentDate());
QString shaFor(const QByteArray &shasums, const QString &file);

// minor -> full version (PHP), major -> full version (Node), from disk.
QMap<QString, QString> installedPhp(const Paths &paths);
QMap<QString, QString> installedNode(const Paths &paths);

QList<Update> updates(const Paths &paths, const Catalog &catalog);

// Blocking network calls with their own event loop; fine for the CLI, which
// is also what the window runs for anything slow.
QByteArray fetch(const QUrl &url, QString *error, int timeoutMs = 30000);
bool download(const QUrl &url, const QString &file, const QString &sha256, const Progress &progress,
              QString *error);
Catalog fetchCatalog(const QString &phpBuild, QString *error);
Catalog cachedCatalog(const Paths &paths);
void saveCatalog(const Paths &paths, const Catalog &catalog);

bool installPhp(const Paths &paths, const PhpRelease &release, const QString &build, const Progress &progress,
                QString *error);
bool installNode(const Paths &paths, const NodeRelease &release, const Progress &progress, QString *error);
bool installComposer(const Paths &paths, QString *error);
bool removePhp(const Paths &paths, const QString &minor);
bool removeNode(const Paths &paths, const QString &major);

} // namespace Runtimes
