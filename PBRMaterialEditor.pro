QT += core gui widgets opengl openglwidgets
CONFIG += c++20 release
TEMPLATE = app
TARGET = "PBR Material Editor"
DESTDIR = bin
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
UI_DIR = build/ui

SOURCES += \
    src/main.cpp \
    src/bgsmreader.cpp \
    src/clicommands.cpp \
    src/converter.cpp \
    src/convertercore.cpp \
    src/convertworkspace.cpp \
    src/maskpaint.cpp \
    src/normalderive.cpp \
    src/materialdocument.cpp \
    src/texturearchive.cpp \
    src/texturecache.cpp \
    src/textureimage.cpp \
    src/texturewriter.cpp \
    src/previewenvironmentsettings.cpp \
    src/tinyexr_impl.cpp \
    src/texturebrowserdialog.cpp \
    src/colorpicker.cpp \
    src/disclosureicon.cpp \
    src/dragnumericedit.cpp \
    src/wheelfocusguard.cpp \
    src/textureslotwidget.cpp \
    src/previewmesh.cpp \
    src/materialpreviewwidget.cpp \
    src/mainwindow.cpp \
    src/mainwindow_pages.cpp \
    src/mainwindow_slots.cpp

HEADERS += \
    src/bgsmreader.h \
    src/clicommands.h \
    src/converter.h \
    src/convertercore.h \
    src/convertworkspace.h \
    src/maskpaint.h \
    src/normalderive.h \
    src/materialdocument.h \
    src/texturearchive.h \
    src/texturecache.h \
    src/textureimage.h \
    src/texturewriter.h \
    src/previewenvironmentsettings.h \
    src/texturebrowserdialog.h \
    src/colorpicker.h \
    src/disclosureicon.h \
    src/dragnumericedit.h \
    src/wheelfocusguard.h \
    src/textureslotwidget.h \
    src/previewmesh.h \
    src/materialpreviewwidget.h \
    src/mainwindow.h \
    src/mainwindow_detail.h

RESOURCES += resources/resources.qrc
RC_ICONS = resources/icons/pbr-material-editor.ico
LIBS += -lz
INCLUDEPATH += third_party/tinyexr

SOURCES += third_party/detex/decompress-bptc.c third_party/detex/bits.c third_party/detex/bptc-tables.c
INCLUDEPATH += third_party/detex
SOURCES += third_party/bc7enc/bc7enc.c
INCLUDEPATH += third_party/bc7enc

exists(third_party/ufbx/ufbx.c) {
    SOURCES += third_party/ufbx/ufbx.c
    INCLUDEPATH += third_party/ufbx
    DEFINES += PBRM_HAS_UFBX
}
