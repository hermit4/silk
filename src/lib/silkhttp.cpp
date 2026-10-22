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

#include "silkhttp.h"
#include "silkserver_logging.h"

#include <QtCore/QDebug>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHttpHeaders>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponder>

#include <zlib.h>

static QByteArray methodName(QHttpServerRequest::Method method)
{
    switch (method) {
    case QHttpServerRequest::Method::Get: return QByteArrayLiteral("GET");
    case QHttpServerRequest::Method::Put: return QByteArrayLiteral("PUT");
    case QHttpServerRequest::Method::Delete: return QByteArrayLiteral("DELETE");
    case QHttpServerRequest::Method::Post: return QByteArrayLiteral("POST");
    case QHttpServerRequest::Method::Head: return QByteArrayLiteral("HEAD");
    case QHttpServerRequest::Method::Options: return QByteArrayLiteral("OPTIONS");
    case QHttpServerRequest::Method::Patch: return QByteArrayLiteral("PATCH");
    case QHttpServerRequest::Method::Connect: return QByteArrayLiteral("CONNECT");
    case QHttpServerRequest::Method::Trace: return QByteArrayLiteral("TRACE");
    default: return QByteArrayLiteral("UNKNOWN");
    }
}

// parameters of a header value such as 'form-data; name="a"; filename="b;c"'
static QHash<QByteArray, QByteArray> headerParameters(const QByteArray &value)
{
    QHash<QByteArray, QByteArray> ret;
    // split at ';' outside of quotes
    QList<QByteArray> fields;
    bool quoted = false;
    qsizetype start = 0;
    for (qsizetype i = 0; i < value.size(); i++) {
        if (value.at(i) == '"') {
            quoted = !quoted;
        } else if (value.at(i) == ';' && !quoted) {
            fields.append(value.mid(start, i - start));
            start = i + 1;
        }
    }
    fields.append(value.mid(start));
    for (qsizetype i = 1; i < fields.size(); i++) {
        const QByteArray field = fields.at(i).trimmed();
        const qsizetype equal = field.indexOf('=');
        if (equal < 0)
            continue;
        QByteArray v = field.mid(equal + 1).trimmed();
        if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
            v = v.mid(1, v.size() - 2);
        ret.insert(field.left(equal).trimmed().toLower(), v);
    }
    return ret;
}

SilkHttpFileData::SilkHttpFileData(const QString &fileName, const QString &contentType, const QByteArray &data, QObject *parent)
    : QBuffer(parent)
    , m_fileName(fileName)
    , m_contentType(contentType)
    , m_data(data)
{
    setBuffer(&m_data);
    open(QIODevice::ReadOnly);
}

SilkHttpRequest::SilkHttpRequest(const QHttpServerRequest &request, QObject *parent)
    : QBuffer(parent)
    , m_uuid(QUuid::createUuid())
    , m_url(request.url())
    , m_method(methodName(request.method()))
    , m_remoteAddress(request.remoteAddress().toString())
    , m_data(request.body())
{
    // qthttpserver used port 80 when the Host header had none; Qt HTTP Server gives 0
    if (m_url.port() <= 0)
        m_url.setPort(80);

    QByteArray multipartBoundary;
    const QHttpHeaders &headers = request.headers();
    for (qsizetype i = 0; i < headers.size(); i++) {
        const QByteArray name = QByteArray(headers.nameAt(i).data(), headers.nameAt(i).size()).toLower();
        const QByteArray value = headers.valueAt(i).toByteArray();
        if (name == "host") {
            // part of url()
        } else if (name == "cookie") {
            for (const QByteArray &c : value.split(';'))
                m_cookies.append(QNetworkCookie::parseCookies(c));
        } else if (name == "content-type" && value.split(';').first().trimmed().toLower() == "multipart/form-data"
                   && !headerParameters(value).value("boundary").isEmpty()) {
            m_rawHeaders.insert(name, QByteArrayLiteral("multipart/form-data"));
            multipartBoundary = headerParameters(value).value("boundary");
        } else {
            m_rawHeaders.insert(name, value);
        }
    }
    if (!multipartBoundary.isEmpty())
        parseMultipart(multipartBoundary);

    setBuffer(&m_data);
    open(QIODevice::ReadOnly);
}

void SilkHttpRequest::parseMultipart(const QByteArray &boundary)
{
    const QByteArray body = m_data;
    const QByteArray delimiter = QByteArrayLiteral("--") + boundary;
    const QByteArray nextDelimiter = QByteArrayLiteral("\r\n") + delimiter;
    QByteArray fields;

    qsizetype pos = body.startsWith(delimiter) ? 0 : body.indexOf(nextDelimiter);
    if (pos < 0) {
        ssWarning() << "multipart boundary not found";
        return;
    }
    pos = body.indexOf(delimiter, pos) + delimiter.size();
    while (body.mid(pos, 2) == "\r\n") {
        pos += 2;
        const qsizetype headerEnd = body.indexOf("\r\n\r\n", pos);
        if (headerEnd < 0)
            break;
        const qsizetype contentEnd = body.indexOf(nextDelimiter, headerEnd + 4);
        if (contentEnd < 0)
            break;

        QHash<QByteArray, QByteArray> partHeaders;
        for (const QByteArray &line : body.mid(pos, headerEnd - pos).split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0)
                partHeaders.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }
        const QByteArray content = body.mid(headerEnd + 4, contentEnd - headerEnd - 4);
        const QHash<QByteArray, QByteArray> disposition = headerParameters(partHeaders.value("content-disposition"));

        if (disposition.contains("filename")) {
            // an empty file input is sent with an empty file name
            if (!disposition.value("filename").isEmpty() && !content.isEmpty()) {
                m_files.append(new SilkHttpFileData(QString::fromUtf8(disposition.value("filename")),
                                                    QString::fromUtf8(partHeaders.value("content-type")),
                                                    content, this));
            }
        } else if (disposition.contains("name")) {
            if (!fields.isEmpty())
                fields.append('&');
            fields.append(QUrl::toPercentEncoding(QString::fromUtf8(disposition.value("name"))));
            fields.append('=');
            fields.append(QUrl::toPercentEncoding(QString::fromUtf8(content)));
        }
        pos = contentEnd + nextDelimiter.size();
    }
    m_data = fields;
}

void SilkHttpRequest::setUrl(const QUrl &url)
{
    if (m_url == url) return;
    m_url = url;
    emit urlChanged(url);
}

SilkHttpReply::SilkHttpReply(const QHttpServerRequest &request, QHttpServerResponder &&responder, QObject *parent)
    : QBuffer(parent)
    , m_responder(new QHttpServerResponder(std::move(responder)))
    , m_head(request.method() == QHttpServerRequest::Method::Head)
{
    for (const QByteArray &encoding : request.value("accept-encoding").split(','))
        m_acceptEncodings.append(encoding.trimmed());
    setBuffer(&m_data);
    open(QIODevice::WriteOnly);
}

SilkHttpReply::~SilkHttpReply() = default;

void SilkHttpReply::setStatus(int status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(status);
}

static QByteArray compress(const QByteArray &source, bool gzip)
{
    z_stream z = {};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, gzip ? 31 : 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return QByteArray();

    QByteArray ret(deflateBound(&z, source.size()), Qt::Uninitialized);
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(source.constData()));
    z.avail_in = source.size();
    z.next_out = reinterpret_cast<Bytef *>(ret.data());
    z.avail_out = ret.size();
    const int status = deflate(&z, Z_FINISH);
    deflateEnd(&z);
    if (status != Z_STREAM_END) {
        ssWarning() << "compression failed:" << status;
        return QByteArray();
    }
    ret.resize(z.total_out);
    return ret;
}

void SilkHttpReply::close()
{
    // e.g. the http protocol handler closes on an upstream error and again when it finishes
    if (m_closed)
        return;
    m_closed = true;
    QBuffer::close();

    // these responses have no body; the connection is kept alive, so stray bytes would be
    // read as the next response
    if (m_head || m_status < 200 || m_status == 204 || m_status == 304)
        m_data.clear();

    if (m_compressionEnabled && !m_data.isEmpty() && !m_rawHeaders.contains("Content-Encoding")) {
        QByteArray encoding;
        if (m_acceptEncodings.contains("gzip"))
            encoding = QByteArrayLiteral("gzip");
        else if (m_acceptEncodings.contains("deflate"))
            encoding = QByteArrayLiteral("deflate");
        if (!encoding.isEmpty()) {
            const QByteArray compressed = compress(m_data, encoding == "gzip");
            if (!compressed.isNull()) {
                m_data = compressed;
                m_rawHeaders.insert("Content-Encoding", encoding);
            }
        }
    }

    QHttpHeaders headers;
    for (auto it = m_rawHeaders.constBegin(); it != m_rawHeaders.constEnd(); ++it) {
        // the responder writes Content-Length itself
        if (it.key().compare("Content-Length", Qt::CaseInsensitive) == 0)
            continue;
        QByteArray value = it.value();
        headers.append(QAnyStringView(it.key()), QAnyStringView(value.replace('\r', "%0D").replace('\n', "%0A")));
    }
    for (const QNetworkCookie &cookie : std::as_const(m_cookies)) {
        QByteArray value = cookie.toRawForm();
        headers.append(QHttpHeaders::WellKnownHeader::SetCookie, QAnyStringView(value.replace('\r', "%0D").replace('\n', "%0A")));
    }
    m_responder->write(m_data, headers, static_cast<QHttpServerResponder::StatusCode>(m_status));
    deleteLater();
}

QDebug operator<<(QDebug dbg, const SilkHttpRequest *request)
{
    if (!request) {
        return dbg << "SilkHttpRequest {}";
    }
    QDebugStateSaver saver(dbg);
    dbg.resetFormat();
    dbg.nospace();
    dbg << "SilkHttpRequest ";
    dbg << "{ uuid: " << request->uuid().toString();
    dbg << "; method: " << request->method();
    dbg << "; url: " << request->url().toString();
    if (request->hasRawHeader("X-Forwarded-For")) {
        dbg << "; ip: " << request->rawHeader("X-Forwarded-For");
    } else {
        dbg << "; ip: " << request->remoteAddress();
    }
    if (request->hasRawHeader("User-Agent")) {
        dbg << "; user_agent: " << request->rawHeader("User-Agent");
    }
    if (request->hasRawHeader("Referer")) {
        dbg << "; referer: " << request->rawHeader("Referer");
    }
    dbg << " }";
    return dbg;
}

QDebug operator<<(QDebug dbg, const SilkWebSocket *socket)
{
    if (!socket) {
        return dbg << "SilkWebSocket {}";
    }
    QDebugStateSaver saver(dbg);
    dbg.resetFormat();
    dbg.nospace();
    dbg << "SilkWebSocket ";
    dbg << "{ uuid: " << socket->uuid().toString();
    dbg << "; url: " << socket->url().toString();
    dbg << "; ip: " << socket->remoteAddress();
    dbg << " }";
    return dbg;
}
