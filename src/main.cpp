#include "engine.hpp"
#include "gui.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "gui") == 0))
        return runGui(argc, argv);
    if (argc == 2 && argv[1][0] != '-') {
        const auto path = QString::fromLocal8Bit(argv[1]);
        if (path.endsWith(u".exe", Qt::CaseInsensitive) || QFileInfo(path).isDir())
            return runGui(argc, argv, true);
    }
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const bool analyze = args.size() == 3 && args[1] == u"analyze";
    const bool build = (args.size() == 3 || (args.size() == 5 && args[3] == u"-o")) && args[1] == u"build";
    QTextStream out(stdout), error(stderr);
    if (!analyze && !build) {
        out << "most [gui | program.exe | folder]\nmost analyze <program.exe>\nmost build <program.exe> [-o <output>]\n";
        return args.size() == 2 && (args[1] == u"--help" || args[1] == u"-h") ? 0 : 2;
    }
    const auto report = most::analyze(args[2]);
    out << report.log() << Qt::endl;
    if (!report.ready())
        return 2;
    if (build) {
        try {
            most::build(report, args.size() == 5 ? args[4] : most::defaultOutput(args[2]), [&](const QString& line) { out << line << Qt::endl; });
        } catch (const std::exception& problem) {
            error << problem.what() << Qt::endl;
            return 1;
        }
    }
    return 0;
}
