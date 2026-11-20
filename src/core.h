#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Every place Loft reads or writes. Tests build one around a temporary home.
struct Paths {
    QString home;
    QString config;   // ~/.config/loft: config.json and each PHP's php.ini
    QString data;     // ~/.local/share/loft: runtimes, shims, nginx, certificates
    QString state;    // ~/.local/state/loft: logs and the last update check
    QString units;    // ~/.config/systemd/user

    // Plain defaults under a home directory, ignoring XDG variables.
    static Paths forHome(const QString &home);
    // The real user's paths, honoring XDG_CONFIG_HOME and friends.
    static Paths current();

    QString configFile() const { return config + "/config.json"; }
    QString phpDir(const QString &minor) const { return data + "/php/" + minor; }
    QString phpConfigDir(const QString &minor) const { return config + "/php/" + minor; }
    QString nodeDir(const QString &major) const { return data + "/node/" + major; }
    QString bin() const { return data + "/bin"; }
    QString npmGlobal() const { return data + "/npm-global"; }
    QString nginx() const { return data + "/nginx"; }
    QString certs() const { return data + "/certs"; }
    QString run() const { return data + "/run"; }
    QString logs() const { return state + "/logs"; }
    QString downloads() const { return data + "/downloads"; }
    QString composer() const { return data + "/composer.phar"; }
    QString phpSocket(const QString &minor) const { return run() + "/php-" + minor + ".sock"; }
    // The table the php and node shims read to pick a version by directory.
    QString versionsFile() const { return data + "/versions.tsv"; }
    QString updatesFile() const { return state + "/updates.json"; }
    QString catalogFile() const { return state + "/catalog.json"; }
};

// What a user changed about one site, whatever made it a site.
struct SiteSettings {
    bool secure = false;
    QString php;    // isolated PHP minor ("8.3"), empty for the default
    QString node;   // isolated Node major ("20"), empty for the default
    bool isEmpty() const { return !secure && php.isEmpty() && node.isEmpty(); }
};

struct Config {
    QString tld = "test";
    QString php;               // default PHP minor, used in the terminal and by sites
    QString node;              // default Node major
    QString phpBuild = "bulk"; // which static-php extension set to download
    bool checkUpdates = true;
    QStringList parked;                    // every folder inside is a site
    QMap<QString, QString> links;          // name -> folder
    QMap<QString, QString> proxies;        // name -> upstream URL
    QMap<QString, SiteSettings> sites;     // name -> settings

    static Config fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
    static Config load(const QString &file);
    bool save(const QString &file, QString *error = nullptr) const;
};

struct Site {
    QString name;     // "blog"
    QString host;     // "blog.test"
    QString path;     // the project folder, empty for a proxy
    QString root;     // nginx's document root
    QString kind;     // "laravel", "php", "static" or "proxy"
    QString source;   // "linked", "parked" or "proxy"
    QString proxy;    // upstream URL of a proxy
    QString php;      // isolated PHP minor, empty when it follows the default
    QString node;     // isolated Node major, empty when it follows the default
    QString phpVersion; // the PHP minor that serves it: isolated or the default
    bool secure = false;
    bool missing = false; // a linked folder that is gone

    QString url() const { return QString(secure ? "https://" : "http://") + host; }
    bool usesPhp() const { return kind == "laravel" || kind == "php"; }
};

// Folder names become host names: lowercase, with anything a host name can't
// hold turned into dashes ("Biomo CRM" -> "biomo-crm").
QString siteName(const QString &folderName);
bool validSiteName(const QString &name);

// What a folder holds and which directory nginx should serve from it.
QString detectKind(const QString &path, QString *root);

// Parked folders first, then links over them, then proxies over both, each
// with its settings applied. Sorted by name.
QList<Site> resolveSites(const Config &config);

// The site whose folder holds directory (the deepest one), or null.
const Site *siteContaining(const QList<Site> &sites, const QString &directory);

QString nginxMainConfig(const Paths &paths, bool ipv6);
QString nginxSiteConfig(const Site &site, const Paths &paths, bool ipv6);
QString phpFpmConfig(const Paths &paths, const QString &minor);
QString defaultPhpIni(const QString &minor, const QString &timezone);

// Lines of "tool<TAB>version<TAB>directory/" for the shims, deepest folders
// first, ending with the defaults under "/".
QString versionsTable(const Config &config, const QList<Site> &sites);

// -1, 0 or 1, comparing dotted numbers ("8.4.9" < "8.4.10").
int compareVersions(const QString &a, const QString &b);
