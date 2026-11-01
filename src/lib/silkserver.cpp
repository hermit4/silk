/* Copyright (c) 2012 Silk Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Silk nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SILK BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "silkserver.h"
#include "silkconfig.h"
#include "silkserver_logging.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QMimeDatabase>
#include <QtCore/QPluginLoader>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponder>

#include <silkhttp.h>

#include <silkmimehandlerinterface.h>
#include <silkabstractmimehandler.h>
#include <silkprotocolhandlerinterface.h>
#include <silkabstractprotocolhandler.h>

typedef QPair<QRegularExpression, QString> RewriteRule;

// Qt HTTP Server can upgrade a connection to WebSocket only after a synchronous decision, and
// cannot send a subprotocol. silk pages decide later (WebSocket.accept(protocol)), so the first
// request of each connection is peeked at here: WebSocket upgrades are taken, everything else
// is passed on to Qt HTTP Server untouched.
class SilkTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    using QTcpServer::QTcpServer;

signals:
    // head: the request line and the header lines, up to the empty line
    void webSocketRequested(QTcpSocket *socket, const QByteArray &head);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    void passOn(QTcpSocket *socket);
};

// Reads the first request head of a connection inside a transaction, so that everything read
// can be given back (rollbackTransaction) before the connection is handed over.
class SilkHeadReader : public QObject
{
    Q_OBJECT
public:
    explicit SilkHeadReader(QTcpSocket *socket);

    QTcpSocket *socket() const { return static_cast<QTcpSocket *>(parent()); }
    // false while the head is incomplete
    bool read(QByteArray *head);
    bool tooLarge() const;

private:
    QByteArray m_data;
    QTimer m_timer;
};

// the limit of Qt's own header parser; beyond it the connection is passed on as it is
static const qint64 maxHeadSize = 256 * 1024;
// for the first request only; Qt HTTP Server 6.8 has no limit of its own
static const int headTimeout = 30 * 1000;

SilkHeadReader::SilkHeadReader(QTcpSocket *socket)
    : QObject(socket)
{
    socket->startTransaction();
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, socket, &QTcpSocket::abort);
    m_timer.start(headTimeout);
}

bool SilkHeadReader::read(QByteArray *head)
{
    // only the new data is read and searched
    qsizetype from = qMax<qsizetype>(0, m_data.size() - 3);
    m_data += socket()->read(maxHeadSize + 1 - m_data.size());
    // like Qt HTTP Server: empty lines before the request line are skipped, and lines may
    // end with LF only
    qsizetype start = 0;
    while (start < m_data.size() && (m_data.at(start) == '\r' || m_data.at(start) == '\n'))
        start++;
    from = qMax(from, start);
    qsizetype end = -1;
    for (const char *terminator : { "\r\n\r\n", "\n\r\n", "\n\n" }) {
        const qsizetype pos = m_data.indexOf(terminator, from);
        if (pos >= 0 && (end < 0 || pos < end))
            end = pos;
    }
    if (end < 0)
        return false;
    *head = m_data.mid(start, end - start);
    return true;
}

bool SilkHeadReader::tooLarge() const
{
    return m_data.size() > maxHeadSize;
}

void SilkTcpServer::incomingConnection(qintptr socketDescriptor)
{
    QTcpSocket *socket = new QTcpSocket(this);
    if (!socket->setSocketDescriptor(socketDescriptor)) {
        delete socket;
        return;
    }
    SilkHeadReader *reader = new SilkHeadReader(socket);
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QTcpSocket::readyRead, reader, [this, reader]() {
        QTcpSocket *socket = reader->socket();
        // The client is gone (e.g. a reset right after the request) and deleteLater() is
        // pending: handing the socket on would let Qt HTTP Server or a page use it after that.
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
        QByteArray head;
        if (!reader->read(&head)) {
            if (reader->tooLarge())
                passOn(socket);
            return;
        }
        bool upgrade = false;
        bool websocket = false;
        for (const QByteArray &line : head.split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed().toLower();
            if (name == "connection" && value.contains("upgrade"))
                upgrade = true;
            else if (name == "upgrade" && value == "websocket")
                websocket = true;
        }
        if (!upgrade || !websocket) {
            passOn(socket);
            return;
        }
        disconnect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        socket->rollbackTransaction();
        delete reader;
        emit webSocketRequested(socket, head);
    });
}

void SilkTcpServer::passOn(QTcpSocket *socket)
{
    disconnect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    socket->rollbackTransaction();
    delete socket->findChild<SilkHeadReader *>();
    addPendingConnection(socket);
    // The request is already buffered: without this Qt HTTP Server would wait for more data.
    // Emitted right away, as Qt does itself: a queued one could arrive after the client has
    // disconnected and the handler has scheduled its own deletion, which it then does while
    // the request is still being answered.
    emit socket->readyRead();
}

class SilkServer::Private : public QObject
{
    Q_OBJECT
public:
    Private(SilkServer *parent);

    void incomingConnection(SilkHttpRequest *request, SilkHttpReply *reply);
    QString clientAddress(const QHostAddress &peer, const QByteArray &forwardedFor) const;
    bool listening = false;

    void incomingConnection(SilkWebSocket *socket);

private:
    QString documentRootForRequest(const QUrl &url) const;
    // conditional: whether If-Modified-Since may turn a static file into 304; not for error pages
    void load(const QFileInfo &fileInfo, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message = QString(), bool conditional = true);
    void loadFile(const QFileInfo &fileInfo, SilkHttpRequest *request, SilkHttpReply *reply, bool conditional);
    void loadUrl(const QUrl &url, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message = QString());
    void load(const QFileInfo &fileInfo, SilkWebSocket *socket, const QString &message = QString());
    void loadUrl(const QUrl &url, SilkWebSocket *socket, const QString &message = QString());

private slots:
    void error(int statusCode, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message = QString());
    void error(int statusCode, SilkWebSocket *socket, const QString &message = QString());

private:
    SilkServer *q;
    QMimeDatabase mimeDatabase;
    QMap<QString, SilkAbstractMimeHandler*> mimeHandlers;
    QMap<QString, SilkAbstractProtocolHandler*> protocolHandlers;
    QList<RewriteRule> rewriteRules;
    QList<QPair<QHostAddress, int>> trustedProxies;
public:
    QHash<QString, QString> documentRoots;
};

SilkServer::Private::Private(SilkServer *parent)
    : QObject(parent)
    , q(parent)
{
    QDir appDir = QCoreApplication::applicationDirPath();
    QDir rootDir = appDir;
    QString appPath(SILK_APP_PATH);
    // up to system root path
    for (int i = 0; i < appPath.count(QLatin1Char('/')) + 1; i++) {
        rootDir.cdUp();
    }
#ifdef QT_STATIC
    {
        foreach (QObject *object, QPluginLoader::staticInstances()) {
            {
                SilkMimeHandlerInterface *plugin = qobject_cast<SilkMimeHandlerInterface *>(object);
                if (plugin) {
                    SilkAbstractMimeHandler *handler = plugin->handler(this);
                    connect(handler, SIGNAL(error(int,SilkHttpRequest*,SilkHttpReply*,QString)), this, SLOT(error(int,SilkHttpRequest*,SilkHttpReply*,QString)));
                    connect(handler, SIGNAL(error(int,SilkWebSocket*,QString)), this, SLOT(error(int,SilkWebSocket*,QString)));
                    foreach (const QString &key, plugin->keys()) {
                        mimeHandlers.insert(key, handler);
                    }
                }
            }
            {
                SilkProtocolHandlerInterface *plugin = qobject_cast<SilkProtocolHandlerInterface *>(object);
                if (plugin) {
                    SilkAbstractProtocolHandler *handler = plugin->handler(this);
                    foreach (const QString &key, plugin->keys()) {
                        protocolHandlers.insert(key, handler);
                    }
                }
            }
        }
    }
#else // QT_STATIC
    {
        QDir pluginsDir = rootDir;
        pluginsDir.cd(SILK_PLUGIN_PATH);
        pluginsDir.cd("mimehandler");
        foreach (const QString &lib, pluginsDir.entryList(QDir::Files)) {
            QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(lib));
            if (pluginLoader.load()) {
                QObject *object = pluginLoader.instance();
                if (object) {
                    SilkMimeHandlerInterface *plugin = qobject_cast<SilkMimeHandlerInterface *>(object);
                    if (plugin) {
                        SilkAbstractMimeHandler *handler = plugin->handler(this);
                        connect(handler, SIGNAL(error(int,SilkHttpRequest*,SilkHttpReply*,QString)), this, SLOT(error(int,SilkHttpRequest*,SilkHttpReply*,QString)));
                        connect(handler, SIGNAL(error(int,SilkWebSocket*,QString)), this, SLOT(error(int,SilkWebSocket*,QString)));
                        foreach (const QString &key, plugin->keys()) {
                            mimeHandlers.insert(key, handler);
                        }
                    } else {
                        qWarning() << object;
                    }
                } else {
                    qWarning() << Q_FUNC_INFO << __LINE__;
                }
            } else {
                qWarning() << pluginLoader.errorString() << pluginsDir.absoluteFilePath(lib);
            }
        }
        pluginsDir.cd("..");

        pluginsDir.cd("protocolhandler");
        foreach (const QString &lib, pluginsDir.entryList(QDir::Files)) {
            QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(lib));
            if (pluginLoader.load()) {
                QObject *object = pluginLoader.instance();
                if (object) {
                    SilkProtocolHandlerInterface *plugin = qobject_cast<SilkProtocolHandlerInterface *>(object);
                    if (plugin) {
                        SilkAbstractProtocolHandler *handler = plugin->handler(this);
                        foreach (const QString &key, plugin->keys()) {
                            protocolHandlers.insert(key, handler);
                        }
                    } else {
                        qWarning() << object;
                    }
                } else {
                    qWarning() << Q_FUNC_INFO << __LINE__;
                }
            } else {
                qWarning() << pluginLoader.errorString() << pluginsDir.absoluteFilePath(lib);
            }
        }
    }
#endif // QT_STATIC

    QString listenSocket = SilkConfig::value("listen.socket").toString();
    QString listenAddress = SilkConfig::value("listen.address").toString();
    QHostAddress address;
    if (!listenSocket.isEmpty()) {
        // listen.address and listen.port are not used
    } else if (listenAddress == QStringLiteral("*")) {
        address = QHostAddress::Any;
    } else if (listenAddress == QStringLiteral("localhost")) {
        address = QHostAddress::LocalHost;
    } else if (!address.setAddress(listenAddress)) {
        qWarning() << "The address" << listenAddress << "is not available.";
        QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
        return;
    }

    int port = SilkConfig::value("listen.port").toInt();

    QVariantMap roots = SilkConfig::value("contents").toMap();
    foreach (const QString &key, roots.keys()) {
        QString value = roots.value(key).toString();
        if (value.contains(":/")) {
            documentRoots.insert(key, value);
        } else {
            QFileInfo fileInfo(value);
            if (fileInfo.isRelative()) {
                if (SilkConfig::url().scheme() == QStringLiteral("qrc")) {
                    documentRoots.insert(key, rootDir.absoluteFilePath(value));
                } else {
                    QDir dir(SilkConfig::url().toLocalFile());
                    dir.cdUp();
                    documentRoots.insert(key, dir.absoluteFilePath(value));
                }
            } else {
                documentRoots.insert(key, value);
            }
        }
    }

    // proxy: { 'trusted': ['172.17.0.1', '10.0.0.0/8'] }; loopback peers are always trusted
    if (SilkConfig::value("proxy").isValid()) {
        QVariant trusted = SilkConfig::value("proxy.trusted");
        // a single address may be given as a string
        if (trusted.userType() == QMetaType::QString)
            trusted = QVariantList { trusted };
        else if (trusted.isValid() && !trusted.canConvert<QVariantList>())
            ssWarning() << "proxy.trusted is not a list:" << trusted;
        foreach (const QVariant &entry, trusted.toList()) {
            QPair<QHostAddress, int> subnet = QHostAddress::parseSubnet(entry.toString());
            if (subnet.first.isNull()) {
                ssWarning() << "proxy.trusted:" << entry.toString() << "is not an address or a subnet";
                continue;
            }
            trustedProxies.append(subnet);
        }
    }

    QVariantMap rewrite = SilkConfig::value("rewrite").toMap();
    foreach (const QString &key, rewrite.keys()) {
        QString value = rewrite.value(key).toString();
        rewriteRules.append(RewriteRule(QRegularExpression(key), value));
    }

    if (!listenSocket.isEmpty()) {
        // the reverse proxy needs write access to the socket file to connect
        QString access = SilkConfig::value("listen.socketAccess").toString();
        QLocalServer::SocketOptions options;
        if (access.isEmpty() || access == QStringLiteral("user")) {
            options = QLocalServer::UserAccessOption;
        } else if (access == QStringLiteral("group")) {
            options = QLocalServer::UserAccessOption | QLocalServer::GroupAccessOption;
        } else if (access == QStringLiteral("world")) {
            options = QLocalServer::WorldAccessOption;
        } else {
            ssWarning() << "listen.socketAccess:" << access << "is not user, group or world.";
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
            return;
        }
        QLocalServer *localServer = new QLocalServer(q);
        localServer->setSocketOptions(options);
        // a socket file left by a previous run would make listen() fail
        QLocalServer::removeServer(listenSocket);
        if (localServer->listen(listenSocket) && q->bind(localServer)) {
            listening = true;
            ssInfo() << "silk is running on" << listenSocket;
        } else {
            ssWarning() << localServer->errorString();
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
        }
    } else {
        SilkTcpServer *tcpServer = new SilkTcpServer(q);
        // Queued: the page is loaded outside of the socket's readyRead, as loading processes
        // events and the client may be gone by then. Nothing deletes the socket in between.
        connect(tcpServer, &SilkTcpServer::webSocketRequested, this, [this](QTcpSocket *socket, const QByteArray &head) {
            if (socket->state() != QAbstractSocket::ConnectedState) {
                socket->deleteLater();
                return;
            }
            SilkWebSocket *webSocket = new SilkWebSocket(socket, head, q);
            webSocket->setRemoteAddress(clientAddress(socket->peerAddress(), webSocket->rawHeader("x-forwarded-for")));
            incomingConnection(webSocket);
        }, Qt::QueuedConnection);
        if (tcpServer->listen(address, port) && q->bind(tcpServer)) {
            listening = true;
            ssInfo() << "silk is running on" << port;
        } else {
            ssWarning() << tcpServer->errorString();
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
        }
    }
}

// The path is appended to the document root, so it must not climb out of it.
// QUrl::path() is decoded: %2e%2e and ..%2f are caught here as well.
static bool isSafePath(const QString &path)
{
    if (path.contains(QLatin1Char('\\')) || path.contains(QChar(0)))
        return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    return !segments.contains(QStringLiteral(".")) && !segments.contains(QStringLiteral(".."));
}

// Nothing in a dot-directory (e.g. .git) and no dot-file is served from a local document
// root, whatever the last segment is. A leading /.well-known/ (RFC 8615, e.g. ACME challenges)
// is the exception; dot-segments below it are still hidden. The path is decoded, so %2e is
// caught as well.
static bool isHiddenPath(const QString &path)
{
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < segments.size(); ++i) {
        const QString &segment = segments.at(i);
        if (segment.startsWith(QLatin1Char('.')) && !(i == 0 && segment == QLatin1String(".well-known")))
            return true;
    }
    return false;
}

// a second line of defense: the file name must stay inside the document root
static bool isInside(const QString &fileName, const QString &documentRoot)
{
    // no document root matched the path: the file name would be the bare path
    if (documentRoot.isEmpty())
        return false;
    const QString root = QDir::cleanPath(documentRoot);
    const QString file = QDir::cleanPath(fileName);
    // cleanPath() keeps the slash of "/"
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return file == root || file.startsWith(prefix);
}

// Location for a directory requested without a trailing slash. Only the path and the query
// are used: the scheme, host and port the client sees belong to the reverse proxy, and the
// Host header is set by the client. A leading "//" would make it a protocol-relative URL.
static QString directoryLocation(const QUrl &url)
{
    QString path = url.path(QUrl::FullyEncoded);
    while (path.startsWith(QLatin1String("//")))
        path.remove(0, 1);
    QString location = path + QLatin1Char('/');
    if (url.hasQuery())
        location += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    return location;
}

// An HTTP-date (RFC 9110, section 5.6.7): IMF-fixdate, or one of the obsolete RFC 850 and
// asctime forms, all in GMT. Anything else gives an invalid QDateTime.
static QDateTime parseHttpDate(const QByteArray &value)
{
    static const QString months = QStringLiteral("Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec");
    static const QString clock = QStringLiteral("(?<hour>\\d{2}):(?<minute>\\d{2}):(?<second>\\d{2})");
    static const QRegularExpression forms[] = {
        // IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
        QRegularExpression(QStringLiteral("^(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), (?<day>\\d{2}) (?<month>%1) (?<year>\\d{4}) %2 GMT$").arg(months, clock)),
        // RFC 850: Sunday, 06-Nov-94 08:49:37 GMT
        QRegularExpression(QStringLiteral("^(?:Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday), (?<day>\\d{2})-(?<month>%1)-(?<year>\\d{2}) %2 GMT$").arg(months, clock)),
        // asctime: Sun Nov  6 08:49:37 1994
        QRegularExpression(QStringLiteral("^(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun) (?<month>%1) (?<day>[ \\d]\\d) %2 (?<year>\\d{4})$").arg(months, clock)),
    };
    static const QStringList monthNames = months.split(QLatin1Char('|'));

    const QString text = QString::fromLatin1(value.trimmed());
    for (const QRegularExpression &form : forms) {
        const QRegularExpressionMatch m = form.match(text);
        if (!m.hasMatch())
            continue;
        int year = m.captured(u"year").toInt();
        if (m.captured(u"year").size() == 2) {
            // a two-digit year more than 50 years in the future is the most recent past year
            // with the same last two digits
            const int thisYear = QDateTime::currentDateTimeUtc().date().year();
            year += thisYear / 100 * 100;
            if (year > thisYear + 50)
                year -= 100;
        }
        const QDate date(year, monthNames.indexOf(m.captured(u"month")) + 1, m.captured(u"day").trimmed().toInt());
        const QTime time(m.captured(u"hour").toInt(), m.captured(u"minute").toInt(), m.captured(u"second").toInt());
        if (!date.isValid() || !time.isValid())
            return QDateTime();
        return QDateTime(date, time, QTimeZone::UTC);
    }
    return QDateTime();
}

// RFC 9110, section 13.1.3, but stricter (see below). silk sends no ETag, so with If-None-Match nothing can match and
// If-Modified-Since must be ignored.
static bool isNotModified(const SilkHttpRequest *request, const QDateTime &lastModified)
{
    if (request->method() != "GET" && request->method() != "HEAD")
        return false;
    if (request->hasRawHeader("If-None-Match") || !request->hasRawHeader("If-Modified-Since"))
        return false;
    const QDateTime since = parseHttpDate(request->rawHeader("If-Modified-Since"));
    if (!since.isValid())
        return false;
    // only the time that was sent as Last-Modified (whole seconds), as nginx does by default
    // (if_modified_since exact): with "not newer than", a file put back to an older version
    // (a rollback, cp -p, docker COPY keep the times) would never reach clients that cached
    // the newer one, and a date in the future would match every file. Answering 200 is
    // always allowed.
    return lastModified.toSecsSinceEpoch() == since.toSecsSinceEpoch();
}

// X-Forwarded-For can be set by anyone; it is only used when the peer is a trusted reverse
// proxy, and then only its last entry, which the proxy itself appended. X-Real-IP is not used:
// a proxy that only appends X-Forwarded-For passes a client's X-Real-IP through.
// A UNIX domain socket peer is reported as loopback.
QString SilkServer::Private::clientAddress(const QHostAddress &peer, const QByteArray &forwardedFor) const
{
    bool trusted = peer.isLoopback();
    // ::ffff:a.b.c.d is the IPv4 peer a.b.c.d
    bool isV4 = false;
    const QHostAddress v4(peer.toIPv4Address(&isV4));
    for (const QPair<QHostAddress, int> &subnet : trustedProxies) {
        if (peer.isInSubnet(subnet) || (isV4 && v4.isInSubnet(subnet)))
            trusted = true;
    }
    if (!trusted)
        return peer.toString();

    const QByteArray forwarded = forwardedFor.split(',').last().trimmed();
    const QHostAddress client(QString::fromLatin1(forwarded));
    return client.isNull() ? peer.toString() : client.toString();
}

QString SilkServer::Private::documentRootForRequest(const QUrl &url) const
{
    QString matched;
    foreach (const QString &key, documentRoots.keys()) {
        if (url.path().startsWith(key) && matched.length() < key.length()) {
            matched = key;
        }
    }

    return documentRoots.value(matched);
}

void SilkServer::Private::incomingConnection(SilkHttpRequest *request, SilkHttpReply *reply)
{
    ssRequest() << request;

    QVariant serverName = SilkConfig::value("server.name");
    if (serverName.isNull()) {
        reply->setRawHeader("Server", "Silk");
    } else {
        QString str = serverName.toString();
        if (!str.isEmpty()) {
            reply->setRawHeader("Server", str.toUtf8());
        }
    }
    QString str = request->url().toString();
    foreach (const RewriteRule &rule, rewriteRules) {
        QRegularExpressionMatch match = rule.first.match(str);
        if (match.hasMatch()) {
            QString url = rule.second;
            for (int i = 0; i <= match.lastCapturedIndex(); i++) {
                url = url.replace(QString("$%1").arg(i), match.captured(i));
            }
            request->setUrl(QUrl(url));
            break;
        }
    }

    if (!isSafePath(request->url().path())) {
        error(400, request, reply);
        return;
    }

    QString documentRoot = documentRootForRequest(request->url());

    if (documentRoot.indexOf("://") > 0) {
        QUrl url(documentRoot);
        url.setPath(request->url().path());
        loadUrl(url, request, reply);
    } else {
        QString fileName(documentRoot + request->url().path());
        if (!isInside(fileName, documentRoot)) {
            error(400, request, reply);
            return;
        }
        if (isHiddenPath(request->url().path())) {
            error(403, request, reply, request->url().toString());
            return;
        }
        QFileInfo fileInfo(fileName);
        if (fileInfo.isDir()) {
            if (request->url().path().endsWith("/")) {
                fileName = fileName + QStringLiteral("/index.qml");
                fileInfo = QFileInfo(fileName);
            } else {
                error(301, request, reply, directoryLocation(request->url()));
                return;
            }
        }

        if (fileInfo.exists()) {
            load(fileInfo, request, reply);
        } else {
            error(404, request, reply, request->url().toString());
            return;
        }
    }
    ssOk() << request << reply->size();
}

void SilkServer::Private::incomingConnection(SilkWebSocket *socket)
{
    QPointer<SilkWebSocket> guard(socket);
    ssRequest() << socket;

    QString str = socket->url().toString();
    foreach (const RewriteRule &rule, rewriteRules) {
        QRegularExpressionMatch match = rule.first.match(str);
        if (match.hasMatch()) {
            QString url = rule.second;
            for (int i = 0; i <= match.lastCapturedIndex(); i++) {
                url = url.replace(QString("$%1").arg(i), match.captured(i));
            }
            socket->setUrl(QUrl(url));
            break;
        }
    }

    if (!isSafePath(socket->url().path())) {
        error(400, socket);
        return;
    }

    QString documentRoot = documentRootForRequest(socket->url());

    if (documentRoot.indexOf("://") > 0) {
        QUrl url(documentRoot);
        url.setPath(socket->url().path());
        loadUrl(url, socket);
    } else {
        QString fileName(documentRoot + socket->url().path());
        if (!isInside(fileName, documentRoot)) {
            error(400, socket);
            return;
        }
        if (isHiddenPath(socket->url().path())) {
            error(403, socket, socket->url().toString());
            return;
        }
        QFileInfo fileInfo(fileName);
        if (fileInfo.isDir()) {
            if (socket->url().path().endsWith("/")) {
                fileName = fileName + QStringLiteral("/index.qml");
                fileInfo = QFileInfo(fileName);
            } else {
                error(301, socket, directoryLocation(socket->url()));
                return;
            }
        }

        if (fileInfo.exists()) {
            load(fileInfo, socket);
        } else {
            error(404, socket, socket->url().toString());
            return;
        }
    }
    // gone if the client disconnected while the page was loaded (it processes events)
    if (guard)
        ssOk() << socket;
}

void SilkServer::Private::load(const QFileInfo &fileInfo, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message, bool conditional)
{
    QStringList mimeTypeListNeedCharset;
    mimeTypeListNeedCharset << QStringLiteral("text/x-qml")
                            << QStringLiteral("text/css")
                            << QStringLiteral("text/html")
                            << QStringLiteral("text/plain")
                            << QStringLiteral("image/svg+xml")
                            << QStringLiteral("text/javascript")
                            << QStringLiteral("application/javascript")
                            << QStringLiteral("application/x-javascript")
                            << QStringLiteral("application/xml")
                            << QStringLiteral("application/atom")
                            << QStringLiteral("application/rss+xml")
                            << QStringLiteral("application/x-shockwave-flash");
    QMimeType mimeType = mimeDatabase.mimeTypeForFile(fileInfo.fileName(), QMimeDatabase::MatchExtension);
    QString mime = mimeType.name();
    QString contentType = mime;
    if (mimeTypeListNeedCharset.contains(mime)) {
        contentType.append(QStringLiteral("; charset=utf-8"));
    }
    reply->setStatus(200);
    reply->setRawHeader("Content-Type", contentType.toUtf8());
    if (!mimeHandlers.contains(mime)) {
        mime = mime.section(QLatin1Char('/'), 0, 0) + QStringLiteral("/*");
    }
    if (mimeHandlers.contains(mime)) {
        QUrl url;
        if (fileInfo.filePath().startsWith(":/")) {
            url = QUrl("qrc" + fileInfo.absoluteFilePath());
        } else {
            url = QUrl::fromLocalFile(fileInfo.absoluteFilePath());
        }
        bool ret = mimeHandlers[mime]->load(url, request, reply, message);
        if (!ret) {
            loadFile(fileInfo, request, reply, conditional);
        }
    } else {
        loadFile(fileInfo, request, reply, conditional);
    }
}

void SilkServer::Private::loadFile(const QFileInfo &fileInfo, SilkHttpRequest *request, SilkHttpReply *reply, bool conditional)
{
    if (fileInfo.fileName().startsWith(".")) {
        error(403, request, reply, request->url().toString());
    } else {
        // TODO cache
        QFile file(fileInfo.absoluteFilePath());
        if (file.open(QFile::ReadOnly)) {
            QDateTime lastModified = fileInfo.lastModified();
            // without a known time (e.g. some resources), never answer 304: the epoch below
            // would match every If-Modified-Since, even after the file changed
            const bool knownTime = lastModified.isValid();
            if (!knownTime) {
                lastModified.setSecsSinceEpoch(0);
            }
            // an HTTP-date (RFC 9110, IMF-fixdate): English names, always in GMT
            reply->setRawHeader("Last-Modified", QLocale::c().toString(lastModified.toUTC(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'")).toUtf8());
            if (conditional && knownTime && isNotModified(request, lastModified)) {
                file.close();
                reply->setStatus(304);
                reply->close();
                return;
            }
            while (!file.atEnd()) {
                reply->write(file.read(1024 * 1024));
            }
            file.close();
            reply->close();
        } else {
            error(403, request, reply, request->url().toString());
        }
    }
}

void SilkServer::Private::loadUrl(const QUrl &url, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message)
{
    bool ret = false;
    if (protocolHandlers.contains(url.scheme())) {
        ret = protocolHandlers[url.scheme()]->load(url, request, reply, message);
    }
    if (!ret) {
        error(403, request, reply, request->url().toString());
    }
}

void SilkServer::Private::error(int statusCode, SilkHttpRequest *request, SilkHttpReply *reply, const QString &message)
{
    ssError() << request << statusCode << message;
    QString documentRoot = documentRootForRequest(request->url());
    if (QFile::exists(QString::fromUtf8("%1/errors/%2.qml").arg(documentRoot).arg(statusCode))) {
        load(QFileInfo(QString::fromUtf8("%1/errors/%2.qml").arg(documentRoot).arg(statusCode)), request, reply, message, false);
    } else if (QFile::exists(QString::fromUtf8(":/errors/%2.qml").arg(statusCode))) {
        load(QFileInfo(QString::fromUtf8(":/errors/%2.qml").arg(statusCode)), request, reply, message, false);
    } else {
        // an application without silk's error pages; loading a missing page would end in
        // another error, over and over
        reply->setStatus(statusCode);
        if (statusCode == 301)
            reply->setRawHeader("Location", message.toUtf8());
        reply->close();
    }
}

void SilkServer::Private::load(const QFileInfo &fileInfo, SilkWebSocket *socket, const QString &message)
{
    QMimeType mimeType = mimeDatabase.mimeTypeForFile(fileInfo.fileName(), QMimeDatabase::MatchExtension);
    QString mime = mimeType.name();
    if (mimeHandlers.contains(mime)) {
        QUrl url;
        if (fileInfo.filePath().startsWith(":/")) {
            url = QUrl("qrc" + fileInfo.absoluteFilePath());
        } else {
            url = QUrl::fromLocalFile(fileInfo.absoluteFilePath());
        }
        QPointer<SilkWebSocket> guard(socket);
        bool ret = mimeHandlers[mime]->load(url, socket, message);
        if (!ret && guard) {
            error(401, socket, socket->url().toString());
        }
    } else {
        error(401, socket, socket->url().toString());
    }
}

void SilkServer::Private::loadUrl(const QUrl &url, SilkWebSocket *socket, const QString &message)
{
    QPointer<SilkWebSocket> guard(socket);
    bool ret = false;
    if (protocolHandlers.contains(url.scheme())) {
        ret = protocolHandlers[url.scheme()]->load(url, socket, message);
    }
    if (!ret && guard) {
        error(403, socket, socket->url().toString());
    }
}

void SilkServer::Private::error(int statusCode, SilkWebSocket *socket, const QString &message)
{
    ssError() << socket << statusCode << message;
    // before the handshake the client still gets an HTTP response
    socket->reject(statusCode, statusCode == 301 ? message.toUtf8() : QByteArray());
}

SilkServer::SilkServer(QObject *parent)
    : QAbstractHttpServer(parent)
    , d(new Private(this))
{
}

SilkServer::~SilkServer()
{
    // replies still pending hold responders whose connections are also children of this
    // object and are destroyed first otherwise
    qDeleteAll(findChildren<SilkHttpReply *>(Qt::FindDirectChildrenOnly));
}

bool SilkServer::isListening() const
{
    return d->listening;
}

bool SilkServer::handleRequest(const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    // upgrades over TCP are taken by SilkTcpServer; Qt WebSockets cannot use a UNIX domain socket
    if (request.value("upgrade").compare("websocket", Qt::CaseInsensitive) == 0) {
        ssWarning() << "WebSocket needs a TCP listener:" << request.url().toString();
        responder.write(QHttpServerResponder::StatusCode::NotImplemented);
        return true;
    }
    // the reply keeps the responder, so that it can be sent later (http.loading)
    SilkHttpReply *reply = new SilkHttpReply(request, std::move(responder), this);
    SilkHttpRequest *silkRequest = new SilkHttpRequest(request, reply);
    silkRequest->setRemoteAddress(d->clientAddress(request.remoteAddress(), request.value("x-forwarded-for")));
    d->incomingConnection(silkRequest, reply);
    return true;
}

void SilkServer::missingHandler(const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    // Reached only for a WebSocket upgrade that is not the first request on its connection:
    // SilkTcpServer did not see it, and Qt HTTP Server has no upgrade handler. Browsers open
    // a new connection for WebSocket. Qt HTTP Server 6.8 closes the connection right after
    // this, so the response does not arrive.
    ssWarning() << "WebSocket upgrade on a used connection:" << request.url().toString();
    responder.write(QHttpServerResponder::StatusCode::BadRequest);
}

QHash<QString, QString> SilkServer::documentRoots() const
{
    return d->documentRoots;
}

void SilkServer::setDocumentRoots(const QHash<QString, QString> &documentRoots)
{
    if (d->documentRoots == documentRoots) return;
    d->documentRoots = documentRoots;
    emit documentRootsChanged(documentRoots);
}

#include "silkserver.moc"
