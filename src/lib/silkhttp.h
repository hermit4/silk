/* Copyright (c) 2026 Silk Project.
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

#ifndef SILKHTTP_H
#define SILKHTTP_H

// Requests and replies on top of the Qt HTTP Server module.
// They keep the interface silk used with the former qthttpserver.

#include "silkglobal.h"

#include <QtCore/QBuffer>
#include <QtCore/QHash>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtNetwork/QNetworkCookie>

#include <memory>

class QHttpServerRequest;
class QHttpServerResponder;
class QDebug;

class SILK_EXPORT SilkHttpFileData : public QBuffer
{
    Q_OBJECT
    Q_PROPERTY(QString fileName READ fileName CONSTANT)
    Q_PROPERTY(QString contentType READ contentType CONSTANT)
public:
    SilkHttpFileData(const QString &fileName, const QString &contentType, const QByteArray &data, QObject *parent = nullptr);

    const QString &fileName() const { return m_fileName; }
    const QString &contentType() const { return m_contentType; }

private:
    QString m_fileName;
    QString m_contentType;
    QByteArray m_data;
};

class SILK_EXPORT SilkHttpRequest : public QBuffer
{
    Q_OBJECT
public:
    explicit SilkHttpRequest(const QHttpServerRequest &request, QObject *parent = nullptr);

    const QUuid &uuid() const { return m_uuid; }
    const QUrl &url() const { return m_url; }
    const QByteArray &method() const { return m_method; }
    // the peer address, or the client address forwarded by a trusted proxy
    const QString &remoteAddress() const { return m_remoteAddress; }
    void setRemoteAddress(const QString &remoteAddress) { m_remoteAddress = remoteAddress; }
    // header names are stored in lower case; Host and Cookie are not included
    bool hasRawHeader(const QByteArray &headerName) const { return m_rawHeaders.contains(headerName.toLower()); }
    QByteArray rawHeader(const QByteArray &headerName) const { return m_rawHeaders.value(headerName.toLower()); }
    QList<QByteArray> rawHeaderList() const { return m_rawHeaders.keys(); }
    const QList<QNetworkCookie> &cookies() const { return m_cookies; }
    // files of a multipart/form-data body; the other fields are the body, url encoded
    const QList<SilkHttpFileData *> &files() const { return m_files; }

public slots:
    void setUrl(const QUrl &url);

signals:
    void urlChanged(const QUrl &url);

private:
    void parseMultipart(const QByteArray &boundary);

    QUuid m_uuid;
    QUrl m_url;
    QByteArray m_method;
    QString m_remoteAddress;
    QHash<QByteArray, QByteArray> m_rawHeaders;
    QList<QNetworkCookie> m_cookies;
    QList<SilkHttpFileData *> m_files;
    QByteArray m_data;
};

class SILK_EXPORT SilkHttpReply : public QBuffer
{
    Q_OBJECT
    Q_PROPERTY(int status READ status WRITE setStatus NOTIFY statusChanged)
public:
    SilkHttpReply(const QHttpServerRequest &request, QHttpServerResponder &&responder, QObject *parent = nullptr);
    ~SilkHttpReply() override;

    int status() const { return m_status; }
    void setStatus(int status);
    bool hasRawHeader(const QByteArray &headerName) const { return m_rawHeaders.contains(headerName); }
    QByteArray rawHeader(const QByteArray &headerName) const { return m_rawHeaders.value(headerName); }
    void setRawHeader(const QByteArray &headerName, const QByteArray &value) { m_rawHeaders.insert(headerName, value); }
    QList<QByteArray> rawHeaderList() const { return m_rawHeaders.keys(); }
    const QList<QNetworkCookie> &cookies() const { return m_cookies; }
    void setCookies(const QList<QNetworkCookie> &cookies) { m_cookies = cookies; }
    // the body is compressed with gzip or deflate when the client accepts it, unless disabled
    // here or a Content-Encoding header is set
    void setCompressionEnabled(bool enabled) { m_compressionEnabled = enabled; }

    // sends the response and deletes this object later
    void close() override;

signals:
    void statusChanged(int status);

private:
    std::unique_ptr<QHttpServerResponder> m_responder;
    QList<QByteArray> m_acceptEncodings;
    bool m_head = false;
    bool m_closed = false;
    int m_status = 200;
    bool m_compressionEnabled = true;
    QHash<QByteArray, QByteArray> m_rawHeaders;
    QList<QNetworkCookie> m_cookies;
    QByteArray m_data;
};

// WebSocket is reimplemented in #5. Until then this class only keeps the WebSocket code
// paths compiling; it is never instantiated.
class SILK_EXPORT SilkWebSocket : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    const QUuid &uuid() const { return m_uuid; }
    const QUrl &url() const { return m_url; }
    void setUrl(const QUrl &url) { m_url = url; }
    const QString &remoteAddress() const { return m_remoteAddress; }
    QByteArray rawHeader(const QByteArray &headerName) const { Q_UNUSED(headerName) return QByteArray(); }
    QList<QByteArray> rawHeaderList() const { return QList<QByteArray>(); }
    const QList<QNetworkCookie> &cookies() const { return m_cookies; }
    void accept(const QByteArray &protocol = QByteArray()) { Q_UNUSED(protocol) }
    void close() {}
    void send(const QByteArray &message) { Q_UNUSED(message) }

signals:
    void message(const QByteArray &message);

private:
    QUuid m_uuid;
    QUrl m_url;
    QString m_remoteAddress;
    QList<QNetworkCookie> m_cookies;
};

SILK_EXPORT QDebug operator<<(QDebug dbg, const SilkHttpRequest *request);
SILK_EXPORT QDebug operator<<(QDebug dbg, const SilkWebSocket *socket);

#endif // SILKHTTP_H
