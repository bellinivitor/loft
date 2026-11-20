#include "core.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace {

QString xdg(const char *variable, const QString &fallback) {
    const QString value = qEnvironmentVariable(variable);
    return value.startsWith('/') ? value : fallback;
}

// nginx reads a quoted string with backslash escapes; folders with a quote or
// a backslash still work this way. ($ can't be escaped, so link refuses it.)
QString quoted(const QString &value) {
    QString escaped = value;
    escaped.replace("\\", "\\\\").replace("\"", "\\\"");
    return "\"" + escaped + "\"";
}

QString canonical(const QString &path) {
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

} // namespace

Paths Paths::forHome(const QString &home) {
    Paths paths;
    paths.home = home;
    paths.config = home + "/.config/loft";
    paths.data = home + "/.local/share/loft";
    paths.state = home + "/.local/state/loft";
    paths.units = home + "/.config/systemd/user";
    return paths;
}

Paths Paths::current() {
    const QString home = QDir::homePath();
    Paths paths = forHome(home);
    const QString config = xdg("XDG_CONFIG_HOME", home + "/.config");
    paths.config = config + "/loft";
    paths.units = config + "/systemd/user";
    paths.data = xdg("XDG_DATA_HOME", home + "/.local/share") + "/loft";
    paths.state = xdg("XDG_STATE_HOME", home + "/.local/state") + "/loft";
    return paths;
}

Config Config::fromJson(const QJsonObject &object) {
    Config config;
    config.tld = object.value("tld").toString("test");
    config.php = object.value("php").toString();
    config.node = object.value("node").toString();
    config.phpBuild = object.value("phpBuild").toString("bulk");
    config.checkUpdates = object.value("checkUpdates").toBool(true);
    for (const auto &value : object.value("parked").toArray())
        if (!value.toString().isEmpty())
            config.parked.append(value.toString());
    const QJsonObject links = object.value("links").toObject();
    for (auto it = links.begin(); it != links.end(); ++it)
        config.links.insert(it.key(), it.value().toString());
    const QJsonObject proxies = object.value("proxies").toObject();
    for (auto it = proxies.begin(); it != proxies.end(); ++it)
        config.proxies.insert(it.key(), it.value().toString());
    const QJsonObject sites = object.value("sites").toObject();
    for (auto it = sites.begin(); it != sites.end(); ++it) {
        const QJsonObject site = it.value().toObject();
        SiteSettings settings;
        settings.secure = site.value("secure").toBool();
        settings.php = site.value("php").toString();
        settings.node = site.value("node").toString();
        if (!settings.isEmpty())
            config.sites.insert(it.key(), settings);
    }
    return config;
}

QJsonObject Config::toJson() const {
    QJsonObject links, proxies, sites;
    for (auto it = this->links.begin(); it != this->links.end(); ++it)
        links.insert(it.key(), it.value());
    for (auto it = this->proxies.begin(); it != this->proxies.end(); ++it)
        proxies.insert(it.key(), it.value());
    for (auto it = this->sites.begin(); it != this->sites.end(); ++it) {
        if (it.value().isEmpty())
            continue;
        QJsonObject site;
        if (it.value().secure)
            site.insert("secure", true);
        if (!it.value().php.isEmpty())
            site.insert("php", it.value().php);
        if (!it.value().node.isEmpty())
            site.insert("node", it.value().node);
        sites.insert(it.key(), site);
    }
    return QJsonObject{
        {"tld", tld},
        {"php", php},
        {"node", node},
        {"phpBuild", phpBuild},
        {"checkUpdates", checkUpdates},
        {"parked", QJsonArray::fromStringList(parked)},
        {"links", links},
        {"proxies", proxies},
        {"sites", sites},
    };
}

Config Config::load(const QString &file) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return Config();
    return fromJson(QJsonDocument::fromJson(input.readAll()).object());
}

bool Config::save(const QString &file, QString *error) const {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile output(file);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = output.errorString();
        return false;
    }
    output.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!output.commit()) {
        if (error)
            *error = output.errorString();
        return false;
    }
    return true;
}

QString siteName(const QString &folderName) {
    QString name = folderName.toLower();
    static const QRegularExpression invalid("[^a-z0-9-]+");
    static const QRegularExpression dashes("-{2,}");
    name.replace(invalid, "-").replace(dashes, "-");
    while (name.startsWith('-'))
        name.remove(0, 1);
    while (name.endsWith('-'))
        name.chop(1);
    return name.left(63);
}

bool validSiteName(const QString &name) {
    static const QRegularExpression valid("^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$");
    return valid.match(name).hasMatch();
}

QString detectKind(const QString &path, QString *root) {
    const QDir dir(path);
    auto has = [&](const QString &file) { return QFileInfo::exists(dir.filePath(file)); };
    QString kind = "static", served = path;
    if (has("public/index.php")) {
        kind = has("artisan") ? "laravel" : "php";
        served = dir.filePath("public");
    } else if (has("index.php")) {
        kind = "php";
    } else if (has("public/index.html")) {
        served = dir.filePath("public");
    }
    if (root)
        *root = served;
    return kind;
}

QList<Site> resolveSites(const Config &config) {
    QMap<QString, Site> byName;

    for (const QString &parked : config.parked) {
        const QDir dir(parked);
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString name = siteName(entry.fileName());
            if (!validSiteName(name) || byName.contains(name))
                continue;
            Site site;
            site.name = name;
            site.path = entry.absoluteFilePath();
            site.source = "parked";
            byName.insert(name, site);
        }
    }

    for (auto it = config.links.begin(); it != config.links.end(); ++it) {
        Site site;
        site.name = it.key();
        site.path = it.value();
        site.source = "linked";
        site.missing = !QFileInfo(it.value()).isDir();
        byName.insert(it.key(), site);
    }

    for (auto it = config.proxies.begin(); it != config.proxies.end(); ++it) {
        Site site;
        site.name = it.key();
        site.kind = "proxy";
        site.source = "proxy";
        site.proxy = it.value();
        byName.insert(it.key(), site);
    }

    QList<Site> sites;
    for (Site site : byName) {
        site.host = site.name + "." + config.tld;
        if (site.kind != "proxy")
            site.kind = detectKind(site.path, &site.root);
        const SiteSettings settings = config.sites.value(site.name);
        site.secure = settings.secure;
        site.php = settings.php;
        site.node = settings.node;
        site.phpVersion = site.php.isEmpty() ? config.php : site.php;
        sites.append(site);
    }
    return sites;
}

const Site *siteContaining(const QList<Site> &sites, const QString &directory) {
    const QString target = canonical(directory) + "/";
    const Site *best = nullptr;
    int bestLength = -1;
    for (const Site &site : sites) {
        if (site.path.isEmpty())
            continue;
        const QString prefix = canonical(site.path) + "/";
        if (target.startsWith(prefix) && prefix.length() > bestLength) {
            best = &site;
            bestLength = prefix.length();
        }
    }
    return best;
}

QString nginxMainConfig(const Paths &paths, bool ipv6) {
    const QString nginx = paths.nginx();
    QStringList lines{
        "# Generated by Loft; it is rewritten on every change. Manage sites with loft.",
        "worker_processes 2;",
        "daemon off;",
        "pid " + quoted(paths.run() + "/nginx.pid") + ";",
        "error_log " + quoted(paths.logs() + "/nginx-error.log") + " warn;",
        "",
        "events {",
        "    worker_connections 1024;",
        "}",
        "",
        "http {",
        "    include /etc/nginx/mime.types;",
        "    default_type application/octet-stream;",
        "    sendfile on;",
        "    tcp_nopush on;",
        "    keepalive_timeout 65;",
        "    gzip on;",
        "    access_log off;",
        "    client_max_body_size 512M;",
        "    server_names_hash_bucket_size 128;",
        "    types_hash_max_size 4096;",
        "",
        "    # nginx runs as you, so its scratch space lives with Loft rather than in /var/lib/nginx.",
        "    client_body_temp_path " + quoted(nginx + "/temp/client_body") + ";",
        "    proxy_temp_path " + quoted(nginx + "/temp/proxy") + ";",
        "    fastcgi_temp_path " + quoted(nginx + "/temp/fastcgi") + ";",
        "    uwsgi_temp_path " + quoted(nginx + "/temp/uwsgi") + ";",
        "    scgi_temp_path " + quoted(nginx + "/temp/scgi") + ";",
        "",
        "    ssl_protocols TLSv1.2 TLSv1.3;",
        "    ssl_session_cache shared:loft:10m;",
        "",
        "    map $http_upgrade $connection_upgrade {",
        "        default upgrade;",
        "        '' close;",
        "    }",
        "",
        "    # Any host Loft doesn't know gets a page saying how to add it.",
        "    server {",
        "        listen 127.0.0.1:80 default_server;",
    };
    if (ipv6)
        lines << "        listen [::1]:80 default_server;";
    lines << "        server_name _;"
          << "        root " + quoted(nginx + "/default") + ";"
          << "        error_page 404 /index.html;"
          << "        location = /index.html { internal; }"
          << "        location / { return 404; }"
          << "    }"
          << ""
          << "    include " + quoted(nginx + "/sites/*.conf") + ";"
          << "}"
          << "";
    return lines.join('\n');
}

QString nginxSiteConfig(const Site &site, const Paths &paths, bool ipv6) {
    const QString names = site.host + " *." + site.host;
    QStringList lines;
    lines << QString("# %1 (%2) — generated by Loft").arg(site.host,
                                                          site.kind == "proxy" ? site.proxy : site.path);

    auto listen = [&](int port, bool ssl) {
        const QString suffix = ssl ? " ssl;" : ";";
        lines << QString("    listen 127.0.0.1:%1").arg(port) + suffix;
        if (ipv6)
            lines << QString("    listen [::1]:%1").arg(port) + suffix;
    };

    if (site.secure) {
        lines << "server {";
        listen(80, false);
        lines << "    server_name " + names + ";"
              << "    return 301 https://$host$request_uri;"
              << "}"
              << "";
    }

    lines << "server {";
    listen(site.secure ? 443 : 80, site.secure);
    if (site.secure)
        lines << "    http2 on;";
    lines << "    server_name " + names + ";";
    if (site.secure)
        lines << "    ssl_certificate " + quoted(paths.certs() + "/" + site.host + ".crt") + ";"
              << "    ssl_certificate_key " + quoted(paths.certs() + "/" + site.host + ".key") + ";";
    lines << "    charset utf-8;";

    if (site.kind == "proxy") {
        lines << "    location / {"
              << "        proxy_pass " + site.proxy + ";"
              << "        proxy_http_version 1.1;"
              << "        proxy_set_header Host $host;"
              << "        proxy_set_header X-Real-IP $remote_addr;"
              << "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;"
              << "        proxy_set_header X-Forwarded-Proto $scheme;"
              << "        proxy_set_header X-Forwarded-Host $host;"
              << "        proxy_set_header Upgrade $http_upgrade;"
              << "        proxy_set_header Connection $connection_upgrade;"
              << "        proxy_buffering off;"
              << "        proxy_read_timeout 600s;"
              << "    }";
    } else if (site.usesPhp()) {
        lines << "    root " + quoted(site.root) + ";"
              << "    index index.php index.html index.htm;"
              << ""
              << "    location / {"
              << "        try_files $uri $uri/ /index.php?$query_string;"
              << "    }"
              << "    location = /favicon.ico { access_log off; log_not_found off; }"
              << "    location = /robots.txt { access_log off; log_not_found off; }"
              << "    location ~ /\\.(?!well-known).* { deny all; }"
              << ""
              << "    location ~ \\.php$ {"
              << "        fastcgi_split_path_info ^(.+\\.php)(/.+)$;"
              << "        fastcgi_pass unix:" + paths.phpSocket(site.phpVersion) + ";"
              << "        fastcgi_index index.php;"
              << "        include /etc/nginx/fastcgi_params;"
              << "        fastcgi_param SCRIPT_FILENAME $realpath_root$fastcgi_script_name;"
              << "        fastcgi_param DOCUMENT_ROOT $realpath_root;"
              << "        fastcgi_param PATH_INFO $fastcgi_path_info;";
        if (site.secure)
            lines << "        fastcgi_param HTTPS on;";
        lines << "        fastcgi_buffers 16 16k;"
              << "        fastcgi_buffer_size 32k;"
              << "        fastcgi_read_timeout 600s;"
              << "    }";
    } else {
        lines << "    root " + quoted(site.root) + ";"
              << "    index index.html index.htm;"
              << "    location / {"
              << "        try_files $uri $uri/ =404;"
              << "    }"
              << "    location ~ /\\.(?!well-known).* { deny all; }";
    }
    lines << "}" << "";
    return lines.join('\n');
}

QString phpFpmConfig(const Paths &paths, const QString &minor) {
    return QStringList{
        QString("; Generated by Loft for PHP %1. PHP settings go in %2/php.ini instead.")
            .arg(minor, paths.phpConfigDir(minor)),
        "[global]",
        "pid = " + paths.run() + "/php-fpm-" + minor + ".pid",
        "error_log = " + paths.logs() + "/php-" + minor + "-fpm.log",
        "daemonize = no",
        "",
        "[loft]",
        "listen = " + paths.phpSocket(minor),
        "listen.mode = 0600",
        "; Workers start on the first request and leave when idle, so an unused",
        "; version costs almost nothing.",
        "pm = ondemand",
        "pm.max_children = 16",
        "pm.process_idle_timeout = 30s",
        "pm.max_requests = 500",
        "catch_workers_output = yes",
        "decorate_workers_output = no",
        "clear_env = no",
        "",
    }.join('\n');
}

QString defaultPhpIni(const QString &minor, const QString &timezone) {
    return QStringList{
        QString("; PHP %1 for Loft, used by the php command and by your sites.").arg(minor),
        "; Edit freely: Loft never rewrites this file. Restart PHP from Loft afterwards.",
        "",
        "memory_limit = 512M",
        "max_execution_time = 300",
        "upload_max_filesize = 512M",
        "post_max_size = 512M",
        "display_errors = On",
        "display_startup_errors = On",
        "error_reporting = E_ALL",
        "variables_order = \"EGPCS\"",
        "date.timezone = " + (timezone.isEmpty() ? QString("UTC") : timezone),
        "",
        "; These builds are static, so they carry no idea where the system keeps",
        "; its certificates. Point them at Arch's bundle, which also holds Loft's CA.",
        "openssl.cafile = /etc/ssl/certs/ca-certificates.crt",
        "curl.cainfo = /etc/ssl/certs/ca-certificates.crt",
        "",
        "opcache.enable = 1",
        "opcache.enable_cli = 0",
        "opcache.validate_timestamps = 1",
        "opcache.revalidate_freq = 0",
        "",
    }.join('\n');
}

QString versionsTable(const Config &config, const QList<Site> &sites) {
    struct Row { QString tool, version, directory; };
    QList<Row> rows;
    for (const Site &site : sites) {
        if (site.path.isEmpty())
            continue;
        // The shims compare against the shell's $PWD, which keeps symlinks, so
        // write both spellings when they differ.
        QStringList directories{QDir::cleanPath(site.path) + "/"};
        const QString real = QFileInfo(site.path).canonicalFilePath();
        if (!real.isEmpty() && real + "/" != directories.first())
            directories << real + "/";
        for (const QString &directory : directories) {
            if (!site.php.isEmpty())
                rows.append({"php", site.php, directory});
            if (!site.node.isEmpty())
                rows.append({"node", site.node, directory});
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.directory.length() > b.directory.length();
    });
    if (!config.php.isEmpty())
        rows.append({"php", config.php, "/"});
    if (!config.node.isEmpty())
        rows.append({"node", config.node, "/"});

    QString table;
    for (const Row &row : rows)
        table += row.tool + "\t" + row.version + "\t" + row.directory + "\n";
    return table;
}

int compareVersions(const QString &a, const QString &b) {
    const QStringList left = a.split('.'), right = b.split('.');
    for (int i = 0; i < std::max(left.size(), right.size()); ++i) {
        const int l = i < left.size() ? left[i].toInt() : 0;
        const int r = i < right.size() ? right[i].toInt() : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}
