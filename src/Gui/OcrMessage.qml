/* SPDX-FileCopyrightText: 2026 Rick Christy <grymmjack@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami

// Status of an OCR (Extract Text) job in the viewer window.
InlineMessage {
    id: root
    property string messageArgument: ""
    property bool error: false
    type: error ? Kirigami.MessageType.Error : Kirigami.MessageType.Information
    text: messageArgument
    actions: Kirigami.Action {
        displayComponent: QQC.ToolButton {
            icon.name: "dialog-close"
            onClicked: root.loader.state = "inactive"
        }
    }
    Timer {
        running: !root.error
        interval: 10000
        onTriggered: root.loader.state = "inactive"
    }
}
