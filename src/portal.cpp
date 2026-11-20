#include "portal.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusObjectPath>
#include <QUrl>

namespace {
const char *Service = "org.freedesktop.portal.Desktop";
const char *Request = "org.freedesktop.portal.Request";
}

FolderPicker::FolderPicker(QObject *parent) : QObject(parent) {}

void FolderPicker::pick(const QString &title) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(QCoreApplication::translate("loft", "The desktop's file picker isn't reachable"));
        return;
    }
    if (!m_requestPath.isEmpty())
        bus.disconnect(Service, m_requestPath, Request, "Response", this, SLOT(response(uint, QVariantMap)));

    // The portal answers on a request object whose path it derives from our
    // bus name and a token we choose; subscribe before asking so the answer
    // can't arrive first.
    const QString token = QString("loft%1").arg(++m_counter);
    const QString sender = bus.baseService().mid(1).replace('.', '_');
    m_requestPath = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    bus.connect(Service, m_requestPath, Request, "Response", this, SLOT(response(uint, QVariantMap)));

    QDBusMessage message = QDBusMessage::createMethodCall(Service, "/org/freedesktop/portal/desktop",
                                                          "org.freedesktop.portal.FileChooser", "OpenFile");
    const QVariantMap options{{"handle_token", token}, {"directory", true}, {"modal", true}};
    message << QString() << title << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        QDBusPendingReply<QDBusObjectPath> reply = *call;
        if (reply.isError())
            emit failed(reply.error().message());
    });
}

void FolderPicker::response(uint code, const QVariantMap &results) {
    if (code != 0)
        return; // cancelled
    const QStringList uris = results.value("uris").toStringList();
    if (uris.isEmpty())
        return;
    const QUrl url(uris.first());
    if (url.isLocalFile())
        emit picked(url.toLocalFile());
}
