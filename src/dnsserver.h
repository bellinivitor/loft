#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUdpSocket>

// The answer to one DNS query: every name under the TLD is this machine
// (A 127.0.0.1, AAAA ::1 when loopback has IPv6), anything else is refused.
// Empty when the packet isn't worth answering.
QByteArray dnsAnswer(const QByteArray &query, const QString &tld, bool ipv6);

// A tiny DNS server on 127.0.0.1. systemd-resolved sends it only *.<tld>,
// through the NetworkManager connection Loft sets up.
class DnsServer : public QObject {
    Q_OBJECT

public:
    DnsServer(const QString &tld, bool ipv6, QObject *parent = nullptr);
    bool listen(quint16 port, QString *error);
    quint16 port() const { return m_socket.localPort(); }

private:
    void read();

    QString m_tld;
    bool m_ipv6;
    QUdpSocket m_socket;
};
