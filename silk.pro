TEMPLATE = subdirs
CONFIG += ordered

include(silk.pri)

!versionCheck(6, 8, 0): error(silk requires Qt 6.8 or later.)

qtHaveModule(qml): qtHaveModule(httpserver) {
    SUBDIRS = src contents demos
} else {
    error(silk requires Qt Qml and Qt HTTP Server installed.)
}
