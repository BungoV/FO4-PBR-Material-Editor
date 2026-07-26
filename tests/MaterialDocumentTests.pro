QT += core gui widgets opengl openglwidgets
CONFIG += c++20 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = MaterialDocumentTests
DESTDIR = ../bin
OBJECTS_DIR = build/obj
MOC_DIR = build/moc

INCLUDEPATH += ../src
SOURCES += materialdocument_tests.cpp ../src/bgsmreader.cpp ../src/converter.cpp ../src/convertercore.cpp ../src/converternodes.cpp ../src/nodegraph.cpp ../src/convertworkspace.cpp ../src/geometryao.cpp ../src/maskpaint.cpp ../src/normalderive.cpp ../src/materialdocument.cpp ../src/texturearchive.cpp ../src/texturecache.cpp ../src/textureimage.cpp ../src/upscaler.cpp ../src/texturewriter.cpp ../src/previewenvironmentsettings.cpp ../src/tinyexr_impl.cpp ../src/texturebrowserdialog.cpp \
    ../src/textureslotwidget.cpp ../src/dragnumericedit.cpp ../src/wheelfocusguard.cpp ../src/disclosureicon.cpp ../src/colorpicker.cpp ../src/previewmesh.cpp ../src/nodecanvas.cpp \
    ../src/materialpreviewwidget.cpp ../src/mainwindow.cpp ../src/mainwindow_pages.cpp ../src/mainwindow_slots.cpp
HEADERS += ../src/materialdocument.h ../src/convertworkspace.h ../src/texturearchive.h ../src/textureimage.h ../src/upscaler.h ../src/previewenvironmentsettings.h ../src/texturebrowserdialog.h ../src/textureslotwidget.h \
    ../src/dragnumericedit.h ../src/wheelfocusguard.h ../src/disclosureicon.h ../src/colorpicker.h ../src/previewmesh.h ../src/materialpreviewwidget.h ../src/mainwindow.h ../src/nodecanvas.h ../src/nodegraph.h

LIBS += -lz
INCLUDEPATH += ../third_party/tinyexr
SOURCES += ../third_party/detex/decompress-bptc.c ../third_party/detex/bits.c ../third_party/detex/bptc-tables.c
INCLUDEPATH += ../third_party/detex
SOURCES += ../third_party/bc7enc/bc7enc.c
INCLUDEPATH += ../third_party/bc7enc

exists(../third_party/ufbx/ufbx.c) {
    SOURCES += ../third_party/ufbx/ufbx.c
    INCLUDEPATH += ../third_party/ufbx
    DEFINES += PBRM_HAS_UFBX
}
