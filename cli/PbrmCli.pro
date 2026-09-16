QT += core gui
CONFIG += c++20 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = pbrm
DESTDIR = ../bin
OBJECTS_DIR = build/obj
MOC_DIR = build/moc

INCLUDEPATH += ../src
SOURCES += main_cli.cpp \
    ../src/bgsmreader.cpp \
    ../src/clicommands.cpp \
    ../src/specularmigrate.cpp \
    ../src/converter.cpp \
    ../src/convertercore.cpp \
    ../src/converternodes.cpp \
    ../src/nodegraph.cpp \
    ../src/geometryao.cpp \
    ../src/maskpaint.cpp \
    ../src/previewmesh.cpp \
    ../src/normalderive.cpp \
    ../src/materialdocument.cpp \
    ../src/texturearchive.cpp \
    ../src/texturecache.cpp \
    ../src/textureimage.cpp \
    ../src/upscaler.cpp \
    ../src/texturewriter.cpp \
    ../src/tinyexr_impl.cpp
HEADERS += ../src/clicommands.h ../src/specularmigrate.h ../src/materialdocument.h ../src/texturearchive.h \
    ../src/texturecache.h ../src/textureimage.h ../src/upscaler.h ../src/texturewriter.h

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
