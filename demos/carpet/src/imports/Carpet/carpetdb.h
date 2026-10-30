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

#ifndef CARPETDB_H
#define CARPETDB_H

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QString>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>

class CarpetDB
{
public:
    static QMutex &mutex();
    static QSqlDatabase &database();
    // the connection for jobs, which run in Backup's thread: Qt 6 does not let a thread use
    // a connection that another thread opened
    static QSqlDatabase jobDatabase();
    static bool exec(QSqlQuery &query);

    // carpet.from and carpet.to in the config; the user's home and ~/.carpet without them
    static QString from();
    static QString to();

    // what QFileInfo::created() returned in Qt 5: the birth time, or the status change time
    static QDateTime created(const QFileInfo &fileInfo) {
        QDateTime ret = fileInfo.birthTime();
        return ret.isValid() ? ret : fileInfo.metadataChangeTime();
    }

private:
    CarpetDB() {}
};

#endif // CARPETDB_H
