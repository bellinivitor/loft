#include "certs.h"
#include "core.h"
#include "dnsserver.h"
#include "loft.h"
#include "runtimes.h"
#include "system.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>

namespace {

void write(const QString &path, const QByteArray &content, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();
    if (executable)
        file.setPermissions(file.permissions() | QFile::ExeOwner);
}

QString read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// A PHP or Node "install" whose binary prints which version ran and the
// php.ini directory it was given, standing in for the real downloads.
void fakePhp(const Paths &paths, const QString &minor, const QString &version) {
    write(paths.phpDir(minor) + "/php", "#!/bin/sh\necho php " + version.toUtf8() + " \"$PHPRC\" \"$@\"\n", true);
    write(paths.phpDir(minor) + "/php-fpm", "#!/bin/sh\n", true);
    write(paths.phpDir(minor) + "/version", version.toUtf8() + "\n");
}

void fakeNode(const Paths &paths, const QString &major, const QString &version) {
    write(paths.nodeDir(major) + "/bin/node", "#!/bin/sh\necho node " + version.toUtf8() + "\n", true);
    write(paths.nodeDir(major) + "/bin/npm", "#!/bin/sh\necho npm " + version.toUtf8() + " \"$NPM_CONFIG_PREFIX\"\n", true);
    write(paths.nodeDir(major) + "/version", version.toUtf8() + "\n");
}

QString runIn(const QString &directory, const QString &program, const QStringList &arguments = {}) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PWD", directory);
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    process.waitForFinished(10000);
    return QString::fromUtf8(process.readAll()).trimmed();
}

QByteArray query(quint16 id, const QByteArray &name, quint16 type) {
    QByteArray packet;
    packet.append(char(id >> 8)).append(char(id & 0xff));
    packet.append('\x01').append('\x00'); // recursion desired
    packet.append(QByteArray("\x00\x01\x00\x00\x00\x00\x00\x00", 8));
    for (const QByteArray &label : name.split('.')) {
        packet.append(char(label.size()));
        packet.append(label);
    }
    packet.append('\0');
    packet.append(char(type >> 8)).append(char(type & 0xff));
    packet.append('\x00').append('\x01');
    return packet;
}

} // namespace

class LoftTests : public QObject {
    Q_OBJECT

private slots:
    void siteNames() {
        QCOMPARE(siteName("Biomo CRM"), QString("biomo-crm"));
        QCOMPARE(siteName("tristar_courier.next"), QString("tristar-courier-next"));
        QCOMPARE(siteName("--Ação--"), QString("a-o"));
        QVERIFY(validSiteName("blog"));
        QVERIFY(validSiteName("my-app2"));
        QVERIFY(!validSiteName("-blog"));
        QVERIFY(!validSiteName("blog.test"));
        QVERIFY(!validSiteName(""));
    }

    void versions() {
        QCOMPARE(compareVersions("8.4.9", "8.4.10"), -1);
        QCOMPARE(compareVersions("8.4.10", "8.4.10"), 0);
        QCOMPARE(compareVersions("24.1.0", "22.23.5"), 1);
        QCOMPARE(compareVersions("8.4", "8.4.0"), 0);
    }

    void kinds() {
        QTemporaryDir dir;
        const QString root = dir.path();
        write(root + "/laravel/artisan", "");
        write(root + "/laravel/public/index.php", "");
        write(root + "/plain/index.php", "");
        write(root + "/spa/public/index.html", "");
        QDir().mkpath(root + "/empty");

        QString served;
        QCOMPARE(detectKind(root + "/laravel", &served), QString("laravel"));
        QCOMPARE(served, root + "/laravel/public");
        QCOMPARE(detectKind(root + "/plain", &served), QString("php"));
        QCOMPARE(served, root + "/plain");
        QCOMPARE(detectKind(root + "/spa", &served), QString("static"));
        QCOMPARE(served, root + "/spa/public");
        QCOMPARE(detectKind(root + "/empty", &served), QString("static"));
        QCOMPARE(served, root + "/empty");
    }

    void configRoundTrip() {
        Config config;
        config.php = "8.4";
        config.node = "24";
        config.parked = {"/home/a/Sites"};
        config.links.insert("blog", "/home/a/blog");
        config.proxies.insert("app", "http://127.0.0.1:3000");
        config.sites.insert("blog", {true, "8.3", "22"});
        config.sites.insert("empty", {});
        const Config back = Config::fromJson(config.toJson());
        QCOMPARE(back.php, QString("8.4"));
        QCOMPARE(back.parked, config.parked);
        QCOMPARE(back.links.value("blog"), QString("/home/a/blog"));
        QCOMPARE(back.proxies.value("app"), QString("http://127.0.0.1:3000"));
        QVERIFY(back.sites.value("blog").secure);
        QCOMPARE(back.sites.value("blog").php, QString("8.3"));
        QVERIFY(!back.sites.contains("empty"));
    }

    void resolution() {
        QTemporaryDir dir;
        const QString root = dir.path();
        write(root + "/Sites/Blog Site/index.php", "");
        write(root + "/Sites/docs/index.html", "");
        write(root + "/elsewhere/docs/public/index.php", "");

        Config config;
        config.php = "8.4";
        config.parked = {root + "/Sites"};
        config.links.insert("docs", root + "/elsewhere/docs");
        config.links.insert("gone", root + "/nope");
        config.proxies.insert("app", "http://127.0.0.1:3000");
        config.sites.insert("blog-site", {true, "8.3", {}});

        const QList<Site> sites = resolveSites(config);
        QCOMPARE(sites.size(), 4);
        QCOMPARE(sites[0].name, QString("app"));
        QCOMPARE(sites[0].kind, QString("proxy"));
        QCOMPARE(sites[1].name, QString("blog-site"));
        QCOMPARE(sites[1].source, QString("parked"));
        QCOMPARE(sites[1].url(), QString("https://blog-site.test"));
        QCOMPARE(sites[1].phpVersion, QString("8.3"));
        // A link overrides the parked folder with the same name.
        QCOMPARE(sites[2].name, QString("docs"));
        QCOMPARE(sites[2].source, QString("linked"));
        QCOMPARE(sites[2].kind, QString("php"));
        QCOMPARE(sites[2].phpVersion, QString("8.4"));
        QVERIFY(sites[3].missing);

        QDir().mkpath(root + "/Sites/Blog Site/app/Models");
        const Site *inside = siteContaining(sites, root + "/Sites/Blog Site/app/Models");
        QVERIFY(inside);
        QCOMPARE(inside->name, QString("blog-site"));
        QVERIFY(!siteContaining(sites, root));
    }

    void nginxConfigs() {
        const Paths paths = Paths::forHome("/home/u");
        Site site;
        site.name = "blog";
        site.host = "blog.test";
        site.path = "/home/u/blog";
        site.root = "/home/u/blog/public";
        site.kind = "laravel";
        site.phpVersion = "8.3";

        QString config = nginxSiteConfig(site, paths, true);
        QVERIFY(config.contains("listen 127.0.0.1:80;"));
        QVERIFY(config.contains("listen [::1]:80;"));
        QVERIFY(config.contains("server_name blog.test *.blog.test;"));
        QVERIFY(config.contains("root \"/home/u/blog/public\";"));
        QVERIFY(config.contains("fastcgi_pass unix:/home/u/.local/share/loft/run/php-8.3.sock;"));
        QVERIFY(!config.contains("ssl"));

        site.secure = true;
        config = nginxSiteConfig(site, paths, false);
        QVERIFY(config.contains("return 301 https://$host$request_uri;"));
        QVERIFY(config.contains("listen 127.0.0.1:443 ssl;"));
        QVERIFY(config.contains("ssl_certificate \"/home/u/.local/share/loft/certs/blog.test.crt\";"));
        QVERIFY(config.contains("fastcgi_param HTTPS on;"));
        QVERIFY(!config.contains("[::1]"));

        Site proxy;
        proxy.name = "app";
        proxy.host = "app.test";
        proxy.kind = "proxy";
        proxy.proxy = "http://127.0.0.1:5173";
        config = nginxSiteConfig(proxy, paths, false);
        QVERIFY(config.contains("proxy_pass http://127.0.0.1:5173;"));
        QVERIFY(config.contains("proxy_set_header Upgrade $http_upgrade;"));

        const QString main = nginxMainConfig(paths, false);
        QVERIFY(main.contains("include \"/home/u/.local/share/loft/nginx/sites/*.conf\";"));
        QVERIFY(main.contains("client_body_temp_path"));
        QVERIFY(main.contains("daemon off;"));

        const QString fpm = phpFpmConfig(paths, "8.3");
        QVERIFY(fpm.contains("listen = /home/u/.local/share/loft/run/php-8.3.sock"));
        QVERIFY(fpm.contains("daemonize = no"));
    }

    void versionTable() {
        Config config;
        config.php = "8.4";
        config.node = "24";
        Site shallow, deep, plain;
        shallow.path = "/w/app";
        shallow.php = "8.2";
        deep.path = "/w/app/packages/admin";
        deep.php = "8.3";
        deep.node = "20";
        plain.path = "/w/other";
        const QString table = versionsTable(config, {shallow, plain, deep});
        QCOMPARE(table, QString("php\t8.3\t/w/app/packages/admin/\n"
                                "node\t20\t/w/app/packages/admin/\n"
                                "php\t8.2\t/w/app/\n"
                                "php\t8.4\t/\n"
                                "node\t24\t/\n"));
    }

    void dnsAnswers() {
        const QByteArray a = dnsAnswer(query(0x1234, "blog.test", 1), "test", true);
        QVERIFY(a.size() > 12);
        QCOMPARE(quint8(a[0]), quint8(0x12));
        QCOMPARE(quint8(a[1]), quint8(0x34));
        QCOMPARE(quint8(a[3]) & 0x0f, 0);          // NOERROR
        QCOMPARE(quint8(a[7]), quint8(1));         // one answer
        QCOMPARE(a.right(4), QByteArray("\x7f\x00\x00\x01", 4));
        QVERIFY(quint8(a[2]) & 0x01);              // recursion desired, copied

        const QByteArray upper = dnsAnswer(query(1, "API.Blog.TEST", 1), "test", true);
        QCOMPARE(upper.right(4), QByteArray("\x7f\x00\x00\x01", 4));

        const QByteArray six = dnsAnswer(query(2, "blog.test", 28), "test", true);
        QCOMPARE(quint8(six[7]), quint8(1));
        QCOMPARE(six.right(16), QByteArray(15, '\0') + '\x01');
        const QByteArray noSix = dnsAnswer(query(3, "blog.test", 28), "test", false);
        QCOMPARE(quint8(noSix[7]), quint8(0));
        QCOMPARE(quint8(noSix[3]) & 0x0f, 0);

        const QByteArray mx = dnsAnswer(query(4, "blog.test", 15), "test", true);
        QCOMPARE(quint8(mx[7]), quint8(0));

        const QByteArray other = dnsAnswer(query(5, "example.com", 1), "test", true);
        QCOMPARE(quint8(other[3]) & 0x0f, 5);      // REFUSED
        const QByteArray notTld = dnsAnswer(query(6, "latest", 1), "test", true);
        QCOMPARE(quint8(notTld[3]) & 0x0f, 5);

        QVERIFY(dnsAnswer(QByteArray("\x00\x01", 2), "test", true).isEmpty());
        QByteArray truncated = query(7, "blog.test", 1);
        truncated.chop(3);
        QCOMPARE(quint8(dnsAnswer(truncated, "test", true)[3]) & 0x0f, 1); // FORMERR
    }

    void dnsServer() {
        DnsServer server("test", false);
        QString error;
        QVERIFY2(server.listen(0, &error), qPrintable(error));
        QUdpSocket client;
        client.writeDatagram(query(9, "shop.test", 1), QHostAddress::LocalHost, server.port());
        QTRY_VERIFY(client.hasPendingDatagrams());
        const QByteArray reply = client.receiveDatagram().data();
        QCOMPARE(reply.right(4), QByteArray("\x7f\x00\x00\x01", 4));
    }

    void catalogs() {
        const QByteArray versions = R"({"latest": {"8.4": "8.4.27", "8.5": "8.5.12"},
            "version-files": {"8.4": "version_84.json"}})";
        const auto latest = Runtimes::parsePhpLatest(versions);
        QCOMPARE(latest.value("8.5"), QString("8.5.12"));

        const QByteArray minor = R"({"minor": "8.4", "php-version": "8.4.27", "artifacts": {
            "linux-x86_64": {"cli": {"file": "php-8.4.27-1-cli-linux-x86_64.tar.gz", "size": 10, "sha256": "aa"},
                             "fpm": {"file": "php-8.4.27-1-fpm-linux-x86_64.tar.gz", "size": 20, "sha256": "bb"}}}})";
        PhpRelease release;
        QVERIFY(Runtimes::parsePhpRelease(minor, "linux-x86_64", &release));
        QCOMPARE(release.version, QString("8.4.27"));
        QCOMPARE(release.fpmFile, QString("php-8.4.27-1-fpm-linux-x86_64.tar.gz"));
        QCOMPARE(release.cliSize + release.fpmSize, qint64(30));
        QVERIFY(!Runtimes::parsePhpRelease(minor, "linux-aarch64", &release));

        const QString platform = Runtimes::nodePlatform();
        const QByteArray index = QString(R"([
            {"version": "v27.0.0-alpha.1", "date": "2026-11-16", "files": ["%1"], "lts": false},
            {"version": "v26.13.1", "date": "2026-11-10", "files": ["%1"], "lts": "Lithium"},
            {"version": "v26.13.0", "date": "2026-11-01", "files": ["%1"], "lts": "Lithium"},
            {"version": "v25.9.0", "date": "2026-03-31", "files": ["%1"], "lts": false},
            {"version": "v24.22.3", "date": "2026-11-10", "files": ["%1"], "lts": "Krypton"},
            {"version": "v20.20.2", "date": "2026-03-24", "files": ["%1"], "lts": "Iron"},
            {"version": "v18.20.8", "date": "2025-03-27", "files": ["%1"], "lts": "Hydrogen"},
            {"version": "v19.0.0", "date": "2022-10-18", "files": ["other"], "lts": false}
        ])").arg(platform).toUtf8();
        const QList<NodeRelease> node = Runtimes::parseNodeIndex(index, QDate(2026, 11, 20));
        QStringList majors;
        for (const NodeRelease &line : node)
            majors << line.major;
        // 25 is neither LTS nor the newest; 18's last release is too old.
        QCOMPARE(majors, QStringList({"26", "24", "20"}));
        QCOMPARE(node.first().version, QString("26.13.1"));
        QCOMPARE(node.first().lts, QString("Lithium"));

        QCOMPARE(Runtimes::shaFor("abc  node-v1-linux-x64.tar.xz\ndef  other\n", "node-v1-linux-x64.tar.xz"),
                 QString("abc"));

        Catalog catalog;
        catalog.php.insert("8.4", release);
        catalog.node = node;
        catalog.fetched = QDateTime::currentDateTimeUtc();
        const Catalog back = Catalog::fromJson(catalog.toJson());
        QCOMPARE(back.php.value("8.4").fpmSha, QString("bb"));
        QCOMPARE(back.nodeRelease("24")->version, QString("24.22.3"));
    }

    void updatesFound() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        fakePhp(paths, "8.4", "8.4.26");
        fakePhp(paths, "8.5", "8.5.12");
        fakeNode(paths, "24", "24.22.3");
        Catalog catalog;
        PhpRelease p84, p85;
        p84.minor = "8.4";
        p84.version = "8.4.27";
        p85.minor = "8.5";
        p85.version = "8.5.12";
        catalog.php.insert("8.4", p84);
        catalog.php.insert("8.5", p85);
        catalog.node = {{"24", "24.23.0", "Krypton", "2026-11-19"}};
        const QList<Update> updates = Runtimes::updates(paths, catalog);
        QCOMPARE(updates.size(), 2);
        QCOMPARE(updates[0].latest, QString("8.4.27"));
        QCOMPARE(updates[1].tool, QString("node"));
    }

    void certificates() {
        if (System::which("openssl").isEmpty())
            QSKIP("openssl is missing");
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        QString error;
        QVERIFY2(Certs::ensureSite(paths, "blog.test", &error), qPrintable(error));
        QVERIFY(Certs::siteValid(paths, "blog.test"));
        QVERIFY(!Certs::siteValid(paths, "shop.test"));
        QCOMPARE(QFile::permissions(Certs::siteKey(paths, "blog.test")) & QFile::ReadOther, QFile::Permissions());
        QCOMPARE(System::run("openssl", {"verify", "-CAfile", Certs::caCert(paths), Certs::siteCert(paths, "blog.test")}), 0);

        const QString bundle = dir.path() + "/bundle.crt";
        write(bundle, "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n");
        QVERIFY(!Certs::trustedBySystem(paths, bundle));
        write(bundle, read(bundle).toUtf8() + read(Certs::caCert(paths)).toUtf8());
        QVERIFY(Certs::trustedBySystem(paths, bundle));

        Certs::removeSite(paths, "blog.test");
        QVERIFY(!Certs::siteValid(paths, "blog.test"));
    }

    void linkSecureIsolate() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        fakePhp(paths, "8.3", "8.3.36");
        fakePhp(paths, "8.4", "8.4.27");
        write(dir.path() + "/Work/blog/artisan", "");
        write(dir.path() + "/Work/blog/public/index.php", "");

        Loft loft(paths, false);
        QVERIFY(loft.useDefault("php", "8.4").ok);
        Result result = loft.link(dir.path() + "/Work/blog", {}, false, {});
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(loft.config().links.value("blog"), dir.path() + "/Work/blog");
        const QString conf = paths.nginx() + "/sites/blog.test.conf";
        QVERIFY(read(conf).contains("php-8.4.sock"));
        QVERIFY(QFileInfo(paths.bin() + "/php").isExecutable());
        QVERIFY(QFileInfo(paths.bin() + "/php8.3").isExecutable());

        // Reloading reads the same config back.
        Loft again(paths, false);
        QCOMPARE(again.sites().size(), 1);

        QVERIFY(!loft.isolate("blog", "php", "8.1").ok); // not installed
        result = loft.isolate("blog", "php", "php@8.3");
        QVERIFY2(result.ok, qPrintable(result.message));
        QVERIFY(read(conf).contains("php-8.3.sock"));

        if (!System::which("openssl").isEmpty()) {
            result = loft.secure("blog", true);
            QVERIFY2(result.ok, qPrintable(result.message));
            QVERIFY(read(conf).contains("listen 127.0.0.1:443 ssl;"));
            QVERIFY(QFileInfo::exists(Certs::siteCert(paths, "blog.test")));
            QVERIFY(loft.secure("blog", false).ok);
            QVERIFY(!QFileInfo::exists(Certs::siteCert(paths, "blog.test")));
        }

        QVERIFY(!loft.uninstallRuntime("php", "8.3").ok); // blog uses it
        QVERIFY(!loft.uninstallRuntime("php", "8.4").ok); // the default

        QVERIFY(loft.unlink("blog").ok);
        QVERIFY(!QFileInfo::exists(conf));
        QVERIFY(!loft.unlink("blog").ok);
    }

    void proxies() {
        QTemporaryDir dir;
        Loft loft(Paths::forHome(dir.path()), false);
        QVERIFY(!loft.proxy("app", "http://127.0.0.1:3000; evil", false).ok);
        QVERIFY(!loft.proxy("app", "ftp://x", false).ok);
        QVERIFY(!loft.proxy("Bad Name", "http://127.0.0.1:3000", false).ok);
        const Result result = loft.proxy("app", "127.0.0.1:5173", false);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(loft.config().proxies.value("app"), QString("http://127.0.0.1:5173"));
        QVERIFY(read(loft.paths().nginx() + "/sites/app.test.conf").contains("proxy_pass http://127.0.0.1:5173;"));
        QVERIFY(!loft.isolate("app", "php", {}).ok);
        QVERIFY(loft.unproxy("app").ok);
        QVERIFY(!QFileInfo::exists(loft.paths().nginx() + "/sites/app.test.conf"));
    }

    void parking() {
        QTemporaryDir dir;
        write(dir.path() + "/Sites/shop/index.html", "");
        Loft loft(Paths::forHome(dir.path()), false);
        QVERIFY(loft.park(dir.path() + "/Sites").ok);
        QVERIFY(QFileInfo::exists(loft.paths().nginx() + "/sites/shop.test.conf"));
        QVERIFY(!loft.unlink("shop").ok); // parked, not linked
        QVERIFY(loft.secure("shop", true).ok || System::which("openssl").isEmpty());
        QVERIFY(loft.forget(dir.path() + "/Sites").ok);
        QVERIFY(!QFileInfo::exists(loft.paths().nginx() + "/sites/shop.test.conf"));
        QVERIFY(!loft.forget(dir.path() + "/Sites").ok);
    }

    void phpShimPicksVersionByFolder() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        fakePhp(paths, "8.3", "8.3.36");
        fakePhp(paths, "8.4", "8.4.27");
        write(dir.path() + "/Work/old/index.php", "");
        write(dir.path() + "/Work/new/index.php", "");
        QDir().mkpath(dir.path() + "/Work/old/src/deep");

        Loft loft(paths, false);
        QVERIFY(loft.useDefault("php", "8.4").ok);
        QVERIFY(loft.link(dir.path() + "/Work/old", {}, false, "8.3").ok);
        QVERIFY(loft.link(dir.path() + "/Work/new", {}, false, {}).ok);

        const QString php = paths.bin() + "/php";
        QCOMPARE(runIn(dir.path() + "/Work/old/src/deep", php, {"-v"}),
                 QString("php 8.3.36 %1/php/8.3 -v").arg(paths.config));
        QCOMPARE(runIn(dir.path() + "/Work/new", php), QString("php 8.4.27 %1/php/8.4").arg(paths.config));
        QCOMPARE(runIn(dir.path(), php), QString("php 8.4.27 %1/php/8.4").arg(paths.config));
        QCOMPARE(runIn(dir.path(), paths.bin() + "/php8.3"), QString("php 8.3.36 %1/php/8.3").arg(paths.config));

        // A folder that only starts with a site's name isn't inside it.
        QDir().mkpath(dir.path() + "/Work/older");
        QCOMPARE(runIn(dir.path() + "/Work/older", php), QString("php 8.4.27 %1/php/8.4").arg(paths.config));
    }

    void nodeShimHonorsNvmrc() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        fakeNode(paths, "22", "22.23.5");
        fakeNode(paths, "24", "24.22.3");
        write(dir.path() + "/Work/web/index.html", "");
        write(dir.path() + "/Work/web/.nvmrc", "v22.1\n");
        write(dir.path() + "/Work/pinned/index.html", "");
        write(dir.path() + "/Work/pinned/.nvmrc", "22\n");
        write(dir.path() + "/Work/wants18/.nvmrc", "18\n");
        QDir().mkpath(dir.path() + "/Work/web/src");

        Loft loft(paths, false);
        QVERIFY(loft.useDefault("node", "24").ok);
        QVERIFY(loft.link(dir.path() + "/Work/pinned", {}, false, {}).ok);
        QVERIFY(loft.isolate("pinned", "node", "24").ok);

        const QString node = paths.bin() + "/node";
        QCOMPARE(runIn(dir.path() + "/Work/web/src", node), QString("node 22.23.5"));
        QCOMPARE(runIn(dir.path() + "/Work/pinned", node), QString("node 24.22.3")); // isolation wins
        QCOMPARE(runIn(dir.path() + "/Work/wants18", node), QString("node 24.22.3")); // 18 isn't installed
        QCOMPARE(runIn(dir.path(), paths.bin() + "/npm"),
                 QString("npm 24.22.3 %1").arg(paths.npmGlobal()));
    }

    void shellSnippetPutsLoftFirst() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        write(dir.path() + "/snippet.bash", System::shellSnippet(paths).toUtf8());
        const QString script = "PATH=/usr/bin:" + paths.bin() + ":/bin; PROMPT_COMMAND=other; source "
                             + dir.path() + "/snippet.bash; _loft_path; echo \"$PATH|$PROMPT_COMMAND\"";
        const QString result = runIn(dir.path(), "bash", {"-c", script});
        const QString path = result.section('|', 0, 0);
        QVERIFY2(path.startsWith(paths.bin() + ":" + paths.npmGlobal() + "/bin:"), qPrintable(path));
        QCOMPARE(path.count(paths.bin() + ":"), 1);
        QCOMPARE(result.section('|', 1), QString("other;_loft_path"));
    }

    void shellConfigIsReversible() {
        QTemporaryDir dir;
        const Paths paths = Paths::forHome(dir.path());
        const QByteArray original = "# mine\nalias ll='ls -l'\n";
        write(dir.path() + "/.bashrc", original);
        QString error;
        QVERIFY(System::configureShell(paths, &error));
        QVERIFY(System::shellConfigured(paths));
        QVERIFY(System::configureShell(paths, &error)); // idempotent
        QCOMPARE(read(dir.path() + "/.bashrc").count("loft.bash"), 2); // the test and the source
        System::unconfigureShell(paths);
        QCOMPARE(read(dir.path() + "/.bashrc"), QString::fromUtf8(original));
    }

    void units() {
        Config config;
        config.parked = {"/home/u/Sites"};
        const auto units = System::unitFiles(Paths::forHome("/home/u"), config, "/usr/bin/loft");
        QVERIFY(units.value("loft-nginx.service").contains("-p /home/u/.local/share/loft/nginx"));
        QVERIFY(units.value("loft-php@.service").contains("/home/u/.local/share/loft/php/%i/php-fpm -F -y"));
        QVERIFY(units.value("loft-dns.service").contains("ExecStart=/usr/bin/loft dns-server"));
        QVERIFY(units.value("loft-parked.path").contains("PathChanged=/home/u/Sites"));
        config.parked.clear();
        QVERIFY(!System::unitFiles(Paths::forHome("/home/u"), config, "/usr/bin/loft").contains("loft-parked.path"));
    }
};

QTEST_GUILESS_MAIN(LoftTests)
#include "loft_tests.moc"
