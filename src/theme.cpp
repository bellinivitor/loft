// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QString Theme::readAccent(const QString &path) {
    return readColors(path).value("accent");
}

QHash<QString, QString> Theme::readColors(const QString &path) {
    QHash<QString, QString> colors;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(R"re(^\s*([a-z_]+)\s*=\s*["']([^"']*)["'])re",
                                            QRegularExpression::MultilineOption);
        auto matches = expression.globalMatch(QString::fromUtf8(file.readAll()));
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString value = match.captured(2);
            if (match.captured(1) == "mode" || QRegularExpression("^#[0-9a-fA-F]{6}$").match(value).hasMatch())
                colors.insert(match.captured(1), value);
        }
    }

    // Without Omarchy: a neutral graphite, warm enough not to read as black.
    if (!colors.contains("accent"))
        colors.insert("accent", "#FFD60A");
    const bool light = colors.value("mode") == "light";
    if (!colors.contains("background"))
        colors.insert("background", light ? "#f4f4f5" : "#1e1f24");
    if (!colors.contains("foreground"))
        colors.insert("foreground", light ? "#27272a" : "#c9cbd3");

    // Missing shades are mixed from the two anchors, so they stay in the
    // theme's own hue.
    const QColor background(colors.value("background")), foreground(colors.value("foreground"));
    auto mix = [&](double amount) {
        return QColor::fromRgbF(background.redF() + (foreground.redF() - background.redF()) * amount,
                                background.greenF() + (foreground.greenF() - background.greenF()) * amount,
                                background.blueF() + (foreground.blueF() - background.blueF()) * amount)
            .name();
    };
    const QList<QPair<QString, QString>> derived{
        {"dark_background", background.darker(light ? 104 : 118).name()},
        {"lighter_background", mix(0.06)},
        {"selection", mix(0.12)},
        {"muted", mix(0.25)},
        {"dark_foreground", mix(0.5)},
        {"bright_foreground", light ? foreground.darker(130).name() : foreground.lighter(115).name()},
    };
    for (const auto &[key, value] : derived)
        if (!colors.contains(key))
            colors.insert(key, value);

    const QList<QPair<QString, QString>> hues{
        {"red", "#f87171"}, {"yellow", "#fbbf24"}, {"green", "#4ade80"}, {"blue", "#60a5fa"},
        {"cyan", "#22d3ee"}, {"magenta", "#c084fc"}, {"orange", "#fb923c"},
    };
    for (const auto &[key, value] : hues)
        if (!colors.contains(key))
            colors.insert(key, value);
    return colors;
}

QString Theme::font() const {
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

QString Theme::tint(const QString &color, double alpha) const {
    QColor c(color);
    c.setAlphaF(qBound(0.0, alpha, 1.0));
    return c.name(QColor::HexArgb);
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(m_accent);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const auto colors = readColors(m_directory + "/theme/colors.toml");
    if (colors != m_colors) {
        m_colors = colors;
        m_accent = colors.value("accent");
        emit changed();
    }
}
