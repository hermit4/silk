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

#include "websocketobject.h"

WebSocketObject::WebSocketObject(QObject *parent)
    : SilkAbstractObject(parent)
    , m_port(80)
{
}

void WebSocketObject::setWebSocket(SilkWebSocket *socket)
{
    m_socket = socket;
    connect(socket, &SilkWebSocket::message, this, &WebSocketObject::onmessage);
    connect(socket, &QObject::destroyed, this, &QObject::deleteLater);
}

void WebSocketObject::accept(const QByteArray &protocol)
{
    if (m_socket)
        m_socket->accept(protocol);
}

void WebSocketObject::close()
{
    if (m_socket)
        m_socket->close();
}

void WebSocketObject::send(const QByteArray &data)
{
    if (m_socket)
        m_socket->send(data);
}

void WebSocketObject::onmessage(const QByteArray &msg, bool binary)
{
    QVariantMap map;
    // a text message is a string in QML, as with the Qt 5 version; a binary one an ArrayBuffer
    if (binary)
        map.insert("data", msg);
    else
        map.insert("data", QString::fromUtf8(msg));
    emit message(map);
}
