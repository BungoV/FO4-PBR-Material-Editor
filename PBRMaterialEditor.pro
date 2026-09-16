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
    src/specularmigrate.cpp \
    src/converter.cpp \
    src/convertercore.cpp \
    src/converternodes.cpp \
    src/convertworkspace.cpp \
    src/nodecanvas.cpp \
    src/nodegraph.cpp \
    src/geometryao.cpp \
    src/maskpaint.cpp \
    src/normalderive.cpp \
    src/materialdocument.cpp \
    src/texturearchive.cpp \
    src/texturecache.cpp \
    src/textureimage.cpp \
    src/upscaler.cpp \
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
    src/specularmigrate.h \
    src/converter.h \
    src/convertercore.h \
    src/converternodes.h \
    src/convertworkspace.h \
    src/nodecanvas.h \
    src/nodegraph.h \
    src/geometryao.h \
    src/maskpaint.h \
    src/normalderive.h \
    src/materialdocument.h \
    src/texturearchive.h \
    src/texturecache.h \
    src/textureimage.h \
    src/upscaler.h \
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

# Diagnostic build: qmake CONFIG+=harden (then make clean, since flag changes
# alone do not invalidate object files). Keeps the release optimisation level so
# timing-sensitive bugs still reproduce, but adds symbols plus Qt's and
# libstdc++'s inline container bounds checks, so an out-of-range index aborts at
# the offending line instead of corrupting the heap and faulting somewhere else.
# _GLIBCXX_ASSERTIONS is ABI-neutral, unlike _GLIBCXX_DEBUG.
# harden implies symbols; set it before the symbols block is evaluated, since
# qmake reads scopes top to bottom and would otherwise skip it.
harden: CONFIG += symbols

# qmake CONFIG+=symbols — debug info only, no codegen change, so a memory bug
# still reproduces with the same heap layout while backtraces name our frames.
# The release spec links with -Wl,-s, which strips the symbol table and reduces
# every one of our frames to "?? ()".
symbols {
    QMAKE_CXXFLAGS += -g
    QMAKE_CFLAGS += -g
    QMAKE_LFLAGS += -g
    QMAKE_LFLAGS_RELEASE -= -Wl,-s
}

# qmake CONFIG+=harden — symbols plus Qt's and libstdc++'s inline container
# bounds checks, so an out-of-range index aborts at the offending line.
# _GLIBCXX_ASSERTIONS is ABI-neutral, unlike _GLIBCXX_DEBUG. Note these checks
# do change inlining and stack layout, which can mask a layout-sensitive bug.
harden {
    QMAKE_CXXFLAGS += -DQT_FORCE_ASSERTS -D_GLIBCXX_ASSERTIONS
}

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
