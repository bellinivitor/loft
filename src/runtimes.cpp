#include "runtimes.h"
#include "system.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTimeZone>
#include <QTimer>
#include <algorithm>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("loft", text); }

QNetworkRequest request(const QUrl &url) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Loft");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

QString readVersion(const QString &directory) {
    QFile file(directory + "/version");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

bool writeVersion(const QString &directory, const QString &version) {
    QFile file(directory + "/version");
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(version.toUtf8() + "\n");
    return true;
}

bool extract(const QString &archive, const QString &directory, const QStringList &extra, QString *error) {
    QDir().mkpath(directory);
    QString output;
    if (System::run("tar", QStringList{"-xf", archive, "-C", directory} + extra, &output, 5 * 60 * 1000) == 0)
        return true;
    if (error)
        *error = tr("Couldn't unpack %1: %2").arg(QFileInfo(archive).fileName(), output.trimmed());
    return false;
}

// Moves a freshly unpacked directory into place, keeping the old one until
// the new one is there, so a failure never leaves a version half-replaced.
bool swapInto(const QString &staging, const QString &target, QString *error) {
    const QString old = QFileInfo(target).absolutePath() + "/." + QFileInfo(target).fileName() + ".old";
    QDir(old).removeRecursively();
    if (QFileInfo::exists(target) && !QDir().rename(target, old)) {
        if (error)
            *error = tr("Can't replace %1").arg(target);
        return false;
    }
    if (!QDir().rename(staging, target)) {
        QDir().rename(old, target);
        if (error)
            *error = tr("Can't move the new version into %1").arg(target);
        return false;
    }
    QDir(old).removeRecursively();
    return true;
}

} // namespace

QJsonObject Catalog::toJson() const {
    QJsonObject php;
    for (const PhpRelease &release : this->php)
        php.insert(release.minor, QJsonObject{
            {"version", release.version},
            {"cliFile", release.cliFile}, {"cliSha", release.cliSha}, {"cliSize", double(release.cliSize)},
            {"fpmFile", release.fpmFile}, {"fpmSha", release.fpmSha}, {"fpmSize", double(release.fpmSize)},
        });
    QJsonArray node;
    for (const NodeRelease &release : this->node)
        node.append(QJsonObject{{"major", release.major}, {"version", release.version},
                                {"lts", release.lts}, {"date", release.date}});
    return QJsonObject{{"php", php}, {"node", node}, {"fetched", fetched.toString(Qt::ISODate)}};
}

Catalog Catalog::fromJson(const QJsonObject &object) {
    Catalog catalog;
    const QJsonObject php = object.value("php").toObject();
    for (auto it = php.begin(); it != php.end(); ++it) {
        const QJsonObject value = it.value().toObject();
        PhpRelease release;
        release.minor = it.key();
        release.version = value.value("version").toString();
        release.cliFile = value.value("cliFile").toString();
        release.cliSha = value.value("cliSha").toString();
        release.cliSize = qint64(value.value("cliSize").toDouble());
        release.fpmFile = value.value("fpmFile").toString();
        release.fpmSha = value.value("fpmSha").toString();
        release.fpmSize = qint64(value.value("fpmSize").toDouble());
        catalog.php.insert(release.minor, release);
    }
    for (const auto &value : object.value("node").toArray()) {
        const QJsonObject item = value.toObject();
        catalog.node.append({item.value("major").toString(), item.value("version").toString(),
                             item.value("lts").toString(), item.value("date").toString()});
    }
    catalog.fetched = QDateTime::fromString(object.value("fetched").toString(), Qt::ISODate);
    return catalog;
}

const NodeRelease *Catalog::nodeRelease(const QString &major) const {
    for (const NodeRelease &release : node)
        if (release.major == major)
            return &release;
    return nullptr;
}

namespace Runtimes {

QString phpPlatform() {
    return QSysInfo::currentCpuArchitecture() == "arm64" ? "linux-aarch64" : "linux-x86_64";
}

QString nodePlatform() {
    return QSysInfo::currentCpuArchitecture() == "arm64" ? "linux-arm64" : "linux-x64";
}

QString phpBase(const QString &build) {
    return "https://dl.static-php.dev/v3/php-bin/" + build + "/";
}

QMap<QString, QString> parsePhpLatest(const QByteArray &versionsJson) {
    QMap<QString, QString> latest;
    const QJsonObject object = QJsonDocument::fromJson(versionsJson).object().value("latest").toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it.value().isString())
            latest.insert(it.key(), it.value().toString());
    return latest;
}

bool parsePhpRelease(const QByteArray &versionJson, const QString &platform, PhpRelease *release) {
    const QJsonObject object = QJsonDocument::fromJson(versionJson).object();
    const QJsonObject artifacts = object.value("artifacts").toObject().value(platform).toObject();
    const QJsonObject cli = artifacts.value("cli").toObject(), fpm = artifacts.value("fpm").toObject();
    if (cli.value("file").toString().isEmpty() || fpm.value("file").toString().isEmpty())
        return false;
    release->minor = object.value("minor").toString();
    release->version = object.value("php-version").toString();
    release->cliFile = cli.value("file").toString();
    release->cliSha = cli.value("sha256").toString();
    release->cliSize = qint64(cli.value("size").toDouble());
    release->fpmFile = fpm.value("file").toString();
    release->fpmSha = fpm.value("sha256").toString();
    release->fpmSize = qint64(fpm.value("size").toDouble());
    return !release->minor.isEmpty() && !release->version.isEmpty();
}

QList<NodeRelease> parseNodeIndex(const QByteArray &indexJson, const QDate &today) {
    QMap<int, NodeRelease> newest;
    const QJsonArray releases = QJsonDocument::fromJson(indexJson).array();
    const QString platform = nodePlatform();
    for (const auto &value : releases) {
        const QJsonObject item = value.toObject();
        const QString version = item.value("version").toString().mid(1); // drop the leading v
        if (version.isEmpty() || version.contains('-'))
            continue;
        bool hasPlatform = false;
        for (const auto &file : item.value("files").toArray())
            hasPlatform = hasPlatform || file.toString() == platform;
        if (!hasPlatform)
            continue;
        const int major = version.section('.', 0, 0).toInt();
        if (newest.contains(major) && compareVersions(newest[major].version, version) >= 0)
            continue;
        const QJsonValue lts = item.value("lts");
        newest.insert(major, {QString::number(major), version, lts.isString() ? lts.toString() : QString(),
                              item.value("date").toString()});
    }

    QList<NodeRelease> lines;
    const int top = newest.isEmpty() ? 0 : newest.lastKey();
    for (auto it = newest.begin(); it != newest.end(); ++it) {
        const bool recent = QDate::fromString(it->date, Qt::ISODate) >= today.addMonths(-18);
        if (it.key() == top || (!it->lts.isEmpty() && recent))
            lines.prepend(it.value());
    }
    return lines;
}

QString shaFor(const QByteArray &shasums, const QString &file) {
    for (const QByteArray &line : shasums.split('\n')) {
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() == 2 && QString::fromUtf8(parts[1]) == file)
            return QString::fromUtf8(parts[0]);
    }
    return {};
}

QMap<QString, QString> installedPhp(const Paths &paths) {
    QMap<QString, QString> installed;
    const auto entries = QDir(paths.data + "/php").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &minor : entries) {
        if (minor.startsWith('.'))
            continue;
        const QString directory = paths.phpDir(minor), version = readVersion(directory);
        if (!version.isEmpty() && QFileInfo(directory + "/php").isExecutable())
            installed.insert(minor, version);
    }
    return installed;
}

QMap<QString, QString> installedNode(const Paths &paths) {
    QMap<QString, QString> installed;
    const auto entries = QDir(paths.data + "/node").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &major : entries) {
        if (major.startsWith('.'))
            continue;
        const QString directory = paths.nodeDir(major), version = readVersion(directory);
        if (!version.isEmpty() && QFileInfo(directory + "/bin/node").isExecutable())
            installed.insert(major, version);
    }
    return installed;
}

QList<Update> updates(const Paths &paths, const Catalog &catalog) {
    QList<Update> list;
    const auto php = installedPhp(paths);
    for (auto it = php.begin(); it != php.end(); ++it)
        if (catalog.php.contains(it.key()) && compareVersions(catalog.php[it.key()].version, it.value()) > 0)
            list.append({"php", it.key(), it.value(), catalog.php[it.key()].version});
    const auto node = installedNode(paths);
    for (auto it = node.begin(); it != node.end(); ++it)
        if (const NodeRelease *release = catalog.nodeRelease(it.key());
            release && compareVersions(release->version, it.value()) > 0)
            list.append({"node", it.key(), it.value(), release->version});
    return list;
}

QByteArray fetch(const QUrl &url, QString *error, int timeoutMs) {
    QNetworkAccessManager manager;
    QNetworkReply *reply = manager.get(request(url));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    const QByteArray body = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError;
    if (!ok && error)
        *error = QString("%1: %2").arg(url.host(), reply->errorString());
    reply->deleteLater();
    return ok ? body : QByteArray();
}

bool download(const QUrl &url, const QString &path, const QString &sha256, const Progress &progress,
              QString *error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QNetworkAccessManager manager;
    QNetworkReply *reply = manager.get(request(url));
    // Stalls abort; a slow but moving download doesn't.
    QTimer stall;
    stall.setSingleShot(true);
    stall.setInterval(60000);
    QObject::connect(&stall, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        const QByteArray chunk = reply->readAll();
        hash.addData(chunk);
        file.write(chunk);
        stall.start();
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 received, qint64 total) {
        if (progress)
            progress(received, total);
    });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    stall.start();
    loop.exec();
    const QByteArray rest = reply->readAll();
    hash.addData(rest);
    file.write(rest);
    file.close();

    QString problem;
    if (reply->error() != QNetworkReply::NoError)
        problem = QString("%1: %2").arg(url.fileName(), reply->errorString());
    else if (!sha256.isEmpty() && hash.result().toHex() != sha256.toLatin1().toLower())
        problem = tr("%1 doesn't match its published checksum").arg(url.fileName());
    reply->deleteLater();
    if (problem.isEmpty())
        return true;
    QFile::remove(path);
    if (error)
        *error = problem;
    return false;
}

Catalog fetchCatalog(const QString &phpBuild, QString *error) {
    Catalog catalog;
    QString problem;
    const QString base = phpBase(phpBuild);
    const QByteArray versions = fetch(QUrl(base + "versions.json"), &problem);
    const auto latest = parsePhpLatest(versions);
    const QJsonObject files = QJsonDocument::fromJson(versions).object().value("version-files").toObject();
    for (auto it = latest.begin(); it != latest.end(); ++it) {
        const QString file = files.value(it.key()).toString();
        if (file.isEmpty())
            continue;
        PhpRelease release;
        if (parsePhpRelease(fetch(QUrl(base + file), &problem), phpPlatform(), &release))
            catalog.php.insert(release.minor, release);
    }
    catalog.node = parseNodeIndex(fetch(QUrl("https://nodejs.org/dist/index.json"), &problem));
    catalog.fetched = QDateTime::currentDateTimeUtc();
    if (catalog.php.isEmpty() || catalog.node.isEmpty()) {
        if (error)
            *error = problem.isEmpty() ? tr("The version lists came back empty") : problem;
        return Catalog();
    }
    return catalog;
}

Catalog cachedCatalog(const Paths &paths) {
    QFile file(paths.catalogFile());
    if (!file.open(QIODevice::ReadOnly))
        return Catalog();
    return Catalog::fromJson(QJsonDocument::fromJson(file.readAll()).object());
}

void saveCatalog(const Paths &paths, const Catalog &catalog) {
    System::writeIfChanged(paths.catalogFile(), QJsonDocument(catalog.toJson()).toJson());
}

bool installPhp(const Paths &paths, const PhpRelease &release, const QString &build, const Progress &progress,
                QString *error) {
    const QString staging = paths.data + "/php/." + release.minor + ".new";
    QDir(staging).removeRecursively();
    const qint64 total = release.cliSize + release.fpmSize;
    qint64 done = 0;
    for (const auto &[file, sha, size] : {std::tuple{release.cliFile, release.cliSha, release.cliSize},
                                          std::tuple{release.fpmFile, release.fpmSha, release.fpmSize}}) {
        const QString archive = paths.downloads() + "/" + file;
        const bool ok = download(QUrl(phpBase(build) + file), archive, sha, [&](qint64 received, qint64) {
            if (progress)
                progress(done + received, total);
        }, error);
        if (!ok || !extract(archive, staging, {}, error)) {
            QFile::remove(archive);
            QDir(staging).removeRecursively();
            return false;
        }
        QFile::remove(archive);
        done += size;
    }
    if (!QFileInfo(staging + "/php").isExecutable() || !QFileInfo(staging + "/php-fpm").isExecutable()) {
        QDir(staging).removeRecursively();
        if (error)
            *error = tr("The download didn't contain php and php-fpm");
        return false;
    }
    writeVersion(staging, release.version);
    System::writeIfChanged(staging + "/php-fpm.conf", phpFpmConfig(paths, release.minor).toUtf8());
    if (!swapInto(staging, paths.phpDir(release.minor), error))
        return false;

    // php.ini is the user's to edit, so it's only written the first time.
    const QString ini = paths.phpConfigDir(release.minor) + "/php.ini";
    if (!QFileInfo::exists(ini))
        System::writeIfChanged(ini, defaultPhpIni(release.minor,
                                                  QString::fromUtf8(QTimeZone::systemTimeZoneId())).toUtf8());
    return true;
}

bool installNode(const Paths &paths, const NodeRelease &release, const Progress &progress, QString *error) {
    const QString base = "https://nodejs.org/dist/v" + release.version + "/";
    const QString file = "node-v" + release.version + "-" + nodePlatform() + ".tar.xz";
    const QString sha = shaFor(fetch(QUrl(base + "SHASUMS256.txt"), error), file);
    if (sha.isEmpty()) {
        if (error && error->isEmpty())
            *error = tr("No published checksum for %1").arg(file);
        return false;
    }
    const QString archive = paths.downloads() + "/" + file;
    const QString staging = paths.data + "/node/." + release.major + ".new";
    QDir(staging).removeRecursively();
    if (!download(QUrl(base + file), archive, sha, progress, error)
        || !extract(archive, staging, {"--strip-components=1"}, error)) {
        QFile::remove(archive);
        QDir(staging).removeRecursively();
        return false;
    }
    QFile::remove(archive);
    if (!QFileInfo(staging + "/bin/node").isExecutable()) {
        QDir(staging).removeRecursively();
        if (error)
            *error = tr("The download didn't contain node");
        return false;
    }
    writeVersion(staging, release.version);
    return swapInto(staging, paths.nodeDir(release.major), error);
}

bool installComposer(const Paths &paths, QString *error) {
    const QString base = "https://getcomposer.org/download/latest-stable/";
    const QString sha = QString::fromUtf8(fetch(QUrl(base + "composer.phar.sha256"), error)).trimmed().section(' ', 0, 0);
    if (sha.length() != 64) {
        if (error && error->isEmpty())
            *error = tr("No published checksum for %1").arg("composer.phar");
        return false;
    }
    const QString temporary = paths.composer() + ".download";
    if (!download(QUrl(base + "composer.phar"), temporary, sha, {}, error))
        return false;
    QFile::remove(paths.composer());
    return QFile::rename(temporary, paths.composer());
}

bool removePhp(const Paths &paths, const QString &minor) {
    return QDir(paths.phpDir(minor)).removeRecursively();
}

bool removeNode(const Paths &paths, const QString &major) {
    return QDir(paths.nodeDir(major)).removeRecursively();
}

} // namespace Runtimes
