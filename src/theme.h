#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    // The rest of the Omarchy palette, so the window sits in the desktop like
    // the terminal does rather than as a black box on top of it.
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString font READ font CONSTANT)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString sunken READ sunken NOTIFY changed)
    Q_PROPERTY(QString surface READ surface NOTIFY changed)
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QString border READ border NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString textBright READ textBright NOTIFY changed)
    Q_PROPERTY(QString textDim READ textDim NOTIFY changed)
    Q_PROPERTY(QString red READ red NOTIFY changed)
    Q_PROPERTY(QString yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QString green READ green NOTIFY changed)
    Q_PROPERTY(QString blue READ blue NOTIFY changed)
    Q_PROPERTY(QString cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QString magenta READ magenta NOTIFY changed)
    Q_PROPERTY(QString orange READ orange NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }

    // Omarchy sets one monospace font for the whole desktop through
    // fontconfig; the window uses it too, like the terminal beside it.
    QString font() const;
    // A color at some opacity, for tinted backgrounds that keep the hue.
    Q_INVOKABLE QString tint(const QString &color, double alpha) const;

    bool dark() const { return m_colors.value("mode") != "light"; }
    QString background() const { return m_colors.value("background"); }
    QString sunken() const { return m_colors.value("dark_background"); }
    QString surface() const { return m_colors.value("lighter_background"); }
    QString selection() const { return m_colors.value("selection"); }
    QString border() const { return m_colors.value("muted"); }
    QString text() const { return m_colors.value("foreground"); }
    QString textBright() const { return m_colors.value("bright_foreground"); }
    QString textDim() const { return m_colors.value("dark_foreground"); }
    QString red() const { return m_colors.value("red"); }
    QString yellow() const { return m_colors.value("yellow"); }
    QString green() const { return m_colors.value("green"); }
    QString blue() const { return m_colors.value("blue"); }
    QString cyan() const { return m_colors.value("cyan"); }
    QString magenta() const { return m_colors.value("magenta"); }
    QString orange() const { return m_colors.value("orange"); }

    static QString readAccent(const QString &path);
    // Every color in colors.toml, with gaps filled from the background and
    // foreground so a theme missing a key still reads as one palette.
    static QHash<QString, QString> readColors(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QHash<QString, QString> m_colors;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
