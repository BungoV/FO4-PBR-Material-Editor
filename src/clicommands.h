#pragma once

#include <QString>
#include <QStringList>

// Headless verb implementations shared by the pbrm.exe console companion and
// the GUI executable's compatibility aliases. Every headless-capable editor
// operation gets a verb; new features ship with theirs (docs/FO4-Conversion.md,
// "CLI parity").
//
// Exit codes: 0 = success / no findings, 1 = findings or operation failures,
// 2 = usage error. Reporting verbs accept --json for machine-readable output.
namespace PbrmCli {

int runVerb(const QString& verb, const QStringList& args);
int runValidate(const QStringList& args);
int runMigrate(const QStringList& args);
int runExtractStates(const QStringList& args);
int runStripLegacy(const QStringList& args);
int runSetSetting(const QStringList& args);
int runRepairPaths(const QStringList& args);
int runExportDefinitions(const QStringList& args);
int runExportJson(const QStringList& args);
int runPack(const QStringList& args);
QString helpText();

} // namespace PbrmCli
