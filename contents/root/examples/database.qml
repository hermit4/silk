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

import QtQml 2.0
import QtQml.Models 2.1
import QtQuick.LocalStorage 2.0
import Silk.HTML 5.0
import Silk.Utils 1.0
import "./components"

Html {
    id: root
    property string error
    property int count

    UserInput {
        id: input

        property string action
        property string value

        onSubmit: function(data) {
            try {
                // stored in a SQLite file under offlineStoragePath of the config
                var db = LocalStorage.openDatabaseSync('examples', '1.0', 'examples/database.qml', 100000);
                db.transaction(function(tx) {
                    tx.executeSql('CREATE TABLE IF NOT EXISTS Chat(key INTEGER PRIMARY KEY, value TEXT)');
                    switch(input.action) {
                    case 'remove':
                        for (var i in data) {
                            if (i.substring(0, 'key_'.length) === 'key_')
                                tx.executeSql('DELETE FROM Chat WHERE key = ?', [parseInt(i.substring('key_'.length))]);
                        }
                        break;
                    case 'insert':
                        tx.executeSql('INSERT INTO Chat(value) VALUES(?)', [Silk.escapeHTML(input.value)]);
                        break;
                    default:
                        break;
                    }
                    var rs = tx.executeSql('SELECT key, value FROM Chat ORDER BY key');
                    for (var j = 0; j < rs.rows.length; j++)
                        chat.append(rs.rows.item(j));
                    root.count = tx.executeSql('SELECT COUNT(key) AS keys FROM Chat WHERE value LIKE ?', ['%Qt%']).rows.item(0).keys;
                });
            } catch (e) {
                root.error = Silk.escapeHTML(e.message);
            }
        }
    }

    ListModel { id: chat }

    Head {
        Title { id: title; text: "Sql Database" }
    }

    Body {
        H1 { text: title.text }
        P { enabled: root.error.length > 0; text: 'error: %1'.arg(root.error) }
        Form {
            method: 'POST'
            Text { text: "value: " }
            Input { type: 'text'; name: 'value' }
            Input { type: 'submit'; value: 'Insert' }
            Input { type: 'hidden'; name: 'action'; value: 'insert' }
        }
        Form {
            method: 'POST'
            Table {
                THead {
                    Tr {
                        Th { text: '&nbsp;' }
                        Th { text: 'key' }
                        Th { text: 'value' }
                    }
                }
                TBody {
                    Repeater {
                        model: chat
                        Component {
                            Tr {
                                Td { Input { type: 'checkbox'; name: 'key_%1'.arg(model.key) } }
                                Td { text: model.key }
                                Td { text: model.value }
                            }
                        }
                    }
                }
                TFoot {
                    Tr {
                        Td {
                            colspan: '3'
                            Input { type: 'submit'; value: 'Remove checked data' }
                            Input { type: 'hidden'; name: 'action'; value: 'remove' }
                        }
                    }
                }
            }
        }

        P { text: 'the number of values that contains "Qt" is %1.'.arg(root.count) }
    }
}
