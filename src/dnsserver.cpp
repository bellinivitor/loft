#include "dnsserver.h"

#include <QHostAddress>
#include <QNetworkDatagram>

namespace {

quint16 read16(const QByteArray &data, int offset) {
    return quint16((quint8(data[offset]) << 8) | quint8(data[offset + 1]));
}

void append16(QByteArray &data, quint16 value) {
    data.append(char(value >> 8));
    data.append(char(value & 0xff));
}

void append32(QByteArray &data, quint32 value) {
    append16(data, quint16(value >> 16));
    append16(data, quint16(value & 0xffff));
}

constexpr quint16 TypeA = 1, TypeAAAA = 28, TypeAny = 255, ClassIn = 1;
constexpr quint16 FormErr = 1, NotImp = 4, Refused = 5;

} // namespace

QByteArray dnsAnswer(const QByteArray &query, const QString &tld, bool ipv6) {
    if (query.size() < 12)
        return {};
    const quint16 id = read16(query, 0), flags = read16(query, 2);
    if (flags & 0x8000) // a response, not a query
        return {};

    auto reply = [&](quint16 rcode, const QByteArray &question, const QList<QByteArray> &answers) {
        QByteArray packet;
        append16(packet, id);
        // QR, the query's opcode, authoritative, its recursion-desired bit,
        // recursion available, and the result code.
        append16(packet, quint16(0x8000 | (flags & 0x7800) | 0x0400 | (flags & 0x0100) | 0x0080 | rcode));
        append16(packet, question.isEmpty() ? 0 : 1);
        append16(packet, quint16(answers.size()));
        append16(packet, 0);
        append16(packet, 0);
        packet += question;
        for (const QByteArray &answer : answers)
            packet += answer;
        return packet;
    };

    if ((flags & 0x7800) != 0)
        return reply(NotImp, {}, {});
    if (read16(query, 4) != 1)
        return reply(FormErr, {}, {});

    // The question: length-prefixed labels up to a zero byte, then type and class.
    QStringList labels;
    int offset = 12;
    while (true) {
        if (offset >= query.size())
            return reply(FormErr, {}, {});
        const int length = quint8(query[offset]);
        if (length == 0)
            break;
        if (length > 63 || offset + 1 + length > query.size())
            return reply(FormErr, {}, {});
        labels << QString::fromLatin1(query.mid(offset + 1, length)).toLower();
        offset += 1 + length;
    }
    offset += 1;
    if (offset + 4 > query.size())
        return reply(FormErr, {}, {});
    const quint16 type = read16(query, offset), klass = read16(query, offset + 2);
    const QByteArray question = query.mid(12, offset + 4 - 12);

    const QString name = labels.join('.');
    const QString suffix = tld.toLower();
    if (name != suffix && !name.endsWith("." + suffix))
        return reply(Refused, question, {});

    QList<QByteArray> answers;
    auto answer = [&](quint16 answerType, const QByteArray &address) {
        QByteArray record;
        append16(record, 0xc00c); // the name, by pointer to the question
        append16(record, answerType);
        append16(record, ClassIn);
        append32(record, 60);
        append16(record, quint16(address.size()));
        record += address;
        answers << record;
    };
    if (klass == ClassIn || klass == TypeAny) {
        if (type == TypeA || type == TypeAny)
            answer(TypeA, QByteArray("\x7f\x00\x00\x01", 4));
        if ((type == TypeAAAA || type == TypeAny) && ipv6)
            answer(TypeAAAA, QByteArray(15, '\0') + '\x01');
    }
    // Any other type exists with no records (NOERROR, no data), so resolvers
    // don't treat the name as missing.
    return reply(0, question, answers);
}

DnsServer::DnsServer(const QString &tld, bool ipv6, QObject *parent)
    : QObject(parent), m_tld(tld), m_ipv6(ipv6) {
    connect(&m_socket, &QUdpSocket::readyRead, this, &DnsServer::read);
}

bool DnsServer::listen(quint16 port, QString *error) {
    if (m_socket.bind(QHostAddress::LocalHost, port))
        return true;
    if (error)
        *error = m_socket.errorString();
    return false;
}

void DnsServer::read() {
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram(4096);
        const QByteArray response = dnsAnswer(datagram.data(), m_tld, m_ipv6);
        if (!response.isEmpty())
            m_socket.writeDatagram(datagram.makeReply(response));
    }
}
