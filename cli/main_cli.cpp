#include "clicommands.h"

#include <QCoreApplication>
#include <QTextStream>

// pbrm — console companion to the FO4 PBR Material Editor. Same core sources
// as the GUI; console subsystem for clean stdout, exit codes, and piping.
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FO4CommunityShaders"));
    QCoreApplication::setApplicationName(QStringLiteral("PBRMaterialEditor"));

    const QStringList arguments = application.arguments();
    if (arguments.size() < 2) {
        QTextStream(stdout) << PbrmCli::helpText() << Qt::flush;
        return 2;
    }
    return PbrmCli::runVerb(arguments.at(1), arguments.mid(2));
}
