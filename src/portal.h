#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Asks xdg-desktop-portal for a folder, so the picker is the desktop's own
// (GTK on Omarchy) rather than Qt's.
class FolderPicker : public QObject {
    Q_OBJECT

public:
    explicit FolderPicker(QObject *parent = nullptr);
    void pick(const QString &title);

signals:
    void picked(const QString &path);
    void failed(const QString &message);

private slots:
    void response(uint code, const QVariantMap &results);

private:
    QString m_requestPath;
    int m_counter = 0;
};
