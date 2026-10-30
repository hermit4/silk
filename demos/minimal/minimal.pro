TARGET = minimal
include(../../silkapp.pri)
include(../../contents/contents.pri)

# demos are built, not installed
INSTALLS -= target

SOURCES += main.cpp

RESOURCES += \
    minimal.qrc

OTHER_FILES += \
    minimalconfig.qml \
    contents/index.qml

