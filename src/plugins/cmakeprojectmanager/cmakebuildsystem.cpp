/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "cmakebuildsystem.h"

#include "builddirparameters.h"
#include "cmakebuildconfiguration.h"
#include "cmakekitinformation.h"
#include "cmakeprojectconstants.h"
#include "cmakeprojectnodes.h"
#include "cmakeprojectplugin.h"
#include "cmakespecificsettings.h"

#include <android/androidconstants.h>
#include <coreplugin/icore.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <cpptools/cppprojectupdater.h>
#include <cpptools/cpptoolsconstants.h>
#include <cpptools/generatedcodemodelsupport.h>
#include <projectexplorer/kitinformation.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/session.h>
#include <projectexplorer/target.h>
#include <projectexplorer/taskhub.h>
#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qtsupport/qtcppkitinfo.h>

#include <app/app_version.h>

#include <utils/checkablemessagebox.h>
#include <utils/fileutils.h>
#include <utils/mimetypes/mimetype.h>
#include <utils/qtcassert.h>

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPushButton>

using namespace ProjectExplorer;
using namespace Utils;

namespace {

void copySourcePathToClipboard(Utils::optional<QString> srcPath,
                               const ProjectExplorer::ProjectNode *node)
{
    QClipboard *clip = QGuiApplication::clipboard();

    QDir projDir{node->filePath().toFileInfo().absoluteFilePath()};
    clip->setText(QDir::cleanPath(projDir.relativeFilePath(srcPath.value())));
}

void noAutoAdditionNotify(const QStringList &filePaths, const ProjectExplorer::ProjectNode *node)
{
    Utils::optional<QString> srcPath{};

    for (const QString &file : filePaths) {
        if (Utils::mimeTypeForFile(file).name() == CppTools::Constants::CPP_SOURCE_MIMETYPE) {
            srcPath = file;
            break;
        }
    }

    if (srcPath) {
        CMakeProjectManager::Internal::CMakeSpecificSettings *settings
            = CMakeProjectManager::Internal::CMakeProjectPlugin::projectTypeSpecificSettings();
        switch (settings->afterAddFileSetting()) {
        case CMakeProjectManager::Internal::ASK_USER: {
            bool checkValue{false};
            QDialogButtonBox::StandardButton reply = Utils::CheckableMessageBox::question(
                nullptr,
                QMessageBox::tr("Copy to Clipboard?"),
                QMessageBox::tr("Files are not automatically added to the "
                                "CMakeLists.txt file of the CMake project."
                                "\nCopy the path to the source files to the clipboard?"),
                "Remember My Choice",
                &checkValue,
                QDialogButtonBox::Yes | QDialogButtonBox::No,
                QDialogButtonBox::Yes);
            if (checkValue) {
                if (QDialogButtonBox::Yes == reply)
                    settings->setAfterAddFileSetting(
                        CMakeProjectManager::Internal::AfterAddFileAction::COPY_FILE_PATH);
                else if (QDialogButtonBox::No == reply)
                    settings->setAfterAddFileSetting(
                        CMakeProjectManager::Internal::AfterAddFileAction::NEVER_COPY_FILE_PATH);

                settings->toSettings(Core::ICore::settings());
            }

            if (QDialogButtonBox::Yes == reply) {
                copySourcePathToClipboard(srcPath, node);
            }
            break;
        }

        case CMakeProjectManager::Internal::COPY_FILE_PATH: {
            copySourcePathToClipboard(srcPath, node);
            break;
        }

        case CMakeProjectManager::Internal::NEVER_COPY_FILE_PATH:
            break;
        }
    }
}

} // namespace

namespace CMakeProjectManager {
namespace Internal {

static Q_LOGGING_CATEGORY(cmakeBuildSystemLog, "qtc.cmake.buildsystem", QtWarningMsg);

// --------------------------------------------------------------------
// CMakeBuildSystem:
// --------------------------------------------------------------------

CMakeBuildSystem::CMakeBuildSystem(CMakeBuildConfiguration *bc)
    : BuildSystem(bc)
    , m_cppCodeModelUpdater(new CppTools::CppProjectUpdater)
{
    // TreeScanner:
    connect(&m_treeScanner, &TreeScanner::finished,
            this, &CMakeBuildSystem::handleTreeScanningFinished);

    m_treeScanner.setFilter([this](const MimeType &mimeType, const FilePath &fn) {
        // Mime checks requires more resources, so keep it last in check list
        auto isIgnored = fn.toString().startsWith(projectFilePath().toString() + ".user")
                         || TreeScanner::isWellKnownBinary(mimeType, fn);

        // Cache mime check result for speed up
        if (!isIgnored) {
            auto it = m_mimeBinaryCache.find(mimeType.name());
            if (it != m_mimeBinaryCache.end()) {
                isIgnored = *it;
            } else {
                isIgnored = TreeScanner::isMimeBinary(mimeType, fn);
                m_mimeBinaryCache[mimeType.name()] = isIgnored;
            }
        }

        return isIgnored;
    });

    m_treeScanner.setTypeFactory([](const Utils::MimeType &mimeType, const Utils::FilePath &fn) {
        auto type = TreeScanner::genericFileType(mimeType, fn);
        if (type == FileType::Unknown) {
            if (mimeType.isValid()) {
                const QString mt = mimeType.name();
                if (mt == CMakeProjectManager::Constants::CMAKE_PROJECT_MIMETYPE
                    || mt == CMakeProjectManager::Constants::CMAKE_MIMETYPE)
                    type = FileType::Project;
            }
        }
        return type;
    });

    connect(&m_reader, &FileApiReader::configurationStarted, this, [this]() {
        cmakeBuildConfiguration()->clearError(CMakeBuildConfiguration::ForceEnabledChanged::True);
    });

    connect(&m_reader,
            &FileApiReader::dataAvailable,
            this,
            &CMakeBuildSystem::handleParsingSucceeded);
    connect(&m_reader, &FileApiReader::errorOccurred, this, &CMakeBuildSystem::handleParsingFailed);
    connect(&m_reader, &FileApiReader::dirty, this, &CMakeBuildSystem::becameDirty);

    connect(SessionManager::instance(),
            &SessionManager::projectAdded,
            this,
            &CMakeBuildSystem::wireUpConnections);
}

CMakeBuildSystem::~CMakeBuildSystem()
{
    if (!m_treeScanner.isFinished()) {
        auto future = m_treeScanner.future();
        future.cancel();
        future.waitForFinished();
    }
    delete m_cppCodeModelUpdater;
    qDeleteAll(m_extraCompilers);
    qDeleteAll(m_allFiles);
}

void CMakeBuildSystem::triggerParsing()
{
    qCDebug(cmakeBuildSystemLog) << "Parsing has been triggered";

    auto guard = guardParsingRun();

    if (!guard.guardsProject()) {
        // This can legitimately trigger if e.g. Build->Run CMake
        // is selected while this here is already running.

        // Stop old parse run and keep that ParseGuard!
        stopParsingAndClearState();
    } else {
        // Use new ParseGuard
        m_currentGuard = std::move(guard);
    }
    QTC_ASSERT(!m_reader.isParsing(), return );

    qCDebug(cmakeBuildSystemLog) << "ParseGuard acquired.";

    if (m_allFiles.isEmpty())
        updateReparseParameters(REPARSE_SCAN);

    m_waitingForScan = (m_reparseParameters | REPARSE_SCAN) != 0;
    m_waitingForParse = true;
    m_combinedScanAndParseResult = true;

    if (m_waitingForScan) {
        qCDebug(cmakeBuildSystemLog) << "Starting TreeScanner";
        QTC_CHECK(m_treeScanner.isFinished());
        m_treeScanner.asyncScanForFiles(projectDirectory());
        Core::ProgressManager::addTask(m_treeScanner.future(),
                                       tr("Scan \"%1\" project tree")
                                           .arg(project()->displayName()),
                                       "CMake.Scan.Tree");
    }

    QTC_ASSERT(m_parameters.isValid(), return );

    TaskHub::clearTasks(ProjectExplorer::Constants::TASK_CATEGORY_BUILDSYSTEM);

    int reparseParameters = takeReparseParameters();

    qCDebug(cmakeBuildSystemLog) << "Parse called with flags:"
                                 << reparseParametersString(reparseParameters);

    const QString cache = m_parameters.workDirectory.pathAppended("CMakeCache.txt").toString();
    if (!QFileInfo::exists(cache)) {
        reparseParameters |= REPARSE_FORCE_CONFIGURATION | REPARSE_FORCE_CMAKE_RUN;
        qCDebug(cmakeBuildSystemLog)
            << "No" << cache
            << "file found, new flags:" << reparseParametersString(reparseParameters);
    } else if (reparseParameters & REPARSE_CHECK_CONFIGURATION) {
        if (checkConfiguration()) {
            reparseParameters |= REPARSE_FORCE_CONFIGURATION | REPARSE_FORCE_CMAKE_RUN;
            qCDebug(cmakeBuildSystemLog) << "Config check triggered flags change:"
                                         << reparseParametersString(reparseParameters);
        }
    }

    writeConfigurationIntoBuildDirectory(m_parameters.expander);

    qCDebug(cmakeBuildSystemLog) << "Asking reader to parse";
    m_reader.parse(reparseParameters & REPARSE_FORCE_CMAKE_RUN,
                   reparseParameters & REPARSE_FORCE_CONFIGURATION);
}

bool CMakeBuildSystem::supportsAction(Node *context, ProjectAction action, const Node *node) const
{
    if (dynamic_cast<CMakeTargetNode *>(context))
        return action == ProjectAction::AddNewFile;

    if (dynamic_cast<CMakeListsNode *>(context))
        return action == ProjectAction::AddNewFile;

    return BuildSystem::supportsAction(context, action, node);
}

bool CMakeBuildSystem::addFiles(Node *context, const QStringList &filePaths, QStringList *notAdded)
{
    if (auto n = dynamic_cast<CMakeProjectNode *>(context)) {
        noAutoAdditionNotify(filePaths, n);
        return true; // Return always true as autoadd is not supported!
    }

    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        noAutoAdditionNotify(filePaths, n);
        return true; // Return always true as autoadd is not supported!
    }

    return BuildSystem::addFiles(context, filePaths, notAdded);
}

QStringList CMakeBuildSystem::filesGeneratedFrom(const QString &sourceFile) const
{
    QFileInfo fi(sourceFile);
    FilePath project = projectDirectory();
    FilePath baseDirectory = FilePath::fromString(fi.absolutePath());

    while (baseDirectory.isChildOf(project)) {
        const FilePath cmakeListsTxt = baseDirectory.pathAppended("CMakeLists.txt");
        if (cmakeListsTxt.exists())
            break;
        baseDirectory = baseDirectory.parentDir();
    }

    QDir srcDirRoot = QDir(project.toString());
    QString relativePath = srcDirRoot.relativeFilePath(baseDirectory.toString());
    QDir buildDir = QDir(cmakeBuildConfiguration()->buildDirectory().toString());
    QString generatedFilePath = buildDir.absoluteFilePath(relativePath);

    if (fi.suffix() == "ui") {
        generatedFilePath += "/ui_";
        generatedFilePath += fi.completeBaseName();
        generatedFilePath += ".h";
        return {QDir::cleanPath(generatedFilePath)};
    }
    if (fi.suffix() == "scxml") {
        generatedFilePath += "/";
        generatedFilePath += QDir::cleanPath(fi.completeBaseName());
        return {generatedFilePath + ".h", generatedFilePath + ".cpp"};
    }

    // TODO: Other types will be added when adapters for their compilers become available.
    return {};
}

QString CMakeBuildSystem::reparseParametersString(int reparseFlags)
{
    QString result;
    if (reparseFlags == REPARSE_DEFAULT) {
        result = "<NONE>";
    } else {
        if (reparseFlags & REPARSE_URGENT)
            result += " URGENT";
        if (reparseFlags & REPARSE_FORCE_CMAKE_RUN)
            result += " FORCE_CMAKE_RUN";
        if (reparseFlags & REPARSE_FORCE_CONFIGURATION)
            result += " FORCE_CONFIG";
        if (reparseFlags & REPARSE_CHECK_CONFIGURATION)
            result += " CHECK_CONFIG";
        if (reparseFlags & REPARSE_SCAN)
            result += " SCAN";
    }
    return result.trimmed();
}

void CMakeBuildSystem::setParametersAndRequestParse(const BuildDirParameters &parameters,
                                                    const int reparseParameters)
{
    qCDebug(cmakeBuildSystemLog) << "setting parameters and requesting reparse";
    if (!parameters.cmakeTool()) {
        TaskHub::addTask(
            BuildSystemTask(Task::Error,
                            tr("The kit needs to define a CMake tool to parse this project.")));
        return;
    }
    QTC_ASSERT(parameters.isValid(), return );

    m_parameters = parameters;
    m_parameters.workDirectory = workDirectory(parameters);
    updateReparseParameters(reparseParameters);

    m_reader.setParameters(m_parameters);

    if (reparseParameters & REPARSE_URGENT) {
        qCDebug(cmakeBuildSystemLog) << "calling requestReparse";
        requestParse();
    } else {
        qCDebug(cmakeBuildSystemLog) << "calling requestDelayedReparse";
        requestDelayedParse();
    }
}

void CMakeBuildSystem::writeConfigurationIntoBuildDirectory(const Utils::MacroExpander *expander)
{
    QTC_ASSERT(expander, return );

    const FilePath buildDir = workDirectory(m_parameters);
    QTC_ASSERT(buildDir.exists(), return );

    const FilePath settingsFile = buildDir.pathAppended("qtcsettings.cmake");

    QByteArray contents;
    contents.append("# This file is managed by Qt Creator, do not edit!\n\n");
    contents.append(
        transform(m_parameters.configuration,
                  [expander](const CMakeConfigItem &item) { return item.toCMakeSetLine(expander); })
            .join('\n')
            .toUtf8());

    QFile file(settingsFile.toString());
    QTC_ASSERT(file.open(QFile::WriteOnly | QFile::Truncate), return );
    file.write(contents);
}

void CMakeBuildSystem::runCMake()
{
    BuildDirParameters parameters(cmakeBuildConfiguration());
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due \"Run CMake\" command";
    setParametersAndRequestParse(parameters,
                                 REPARSE_CHECK_CONFIGURATION | REPARSE_FORCE_CMAKE_RUN
                                     | REPARSE_URGENT);
}

void CMakeBuildSystem::runCMakeAndScanProjectTree()
{
    BuildDirParameters parameters(cmakeBuildConfiguration());
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due to \"Rescan Project\" command";
    setParametersAndRequestParse(parameters, REPARSE_CHECK_CONFIGURATION | REPARSE_SCAN);
}

void CMakeBuildSystem::buildCMakeTarget(const QString &buildTarget)
{
    QTC_ASSERT(!buildTarget.isEmpty(), return);
    cmakeBuildConfiguration()->buildTarget(buildTarget);
}

void CMakeBuildSystem::handleTreeScanningFinished()
{
    QTC_CHECK(m_waitingForScan);

    qDeleteAll(m_allFiles);
    m_allFiles = Utils::transform(m_treeScanner.release(), [](const FileNode *fn) { return fn; });

    m_waitingForScan = false;

    combineScanAndParse();
}

bool CMakeBuildSystem::persistCMakeState()
{
    QTC_ASSERT(m_parameters.isValid(), return false);

    if (m_parameters.workDirectory == m_parameters.buildDirectory)
        return false;

    if (!buildConfiguration()->createBuildDirectory())
        return false;

    BuildDirParameters newParameters = m_parameters;
    newParameters.workDirectory.clear();
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due to persisting CMake State";
    setParametersAndRequestParse(newParameters,
                                 REPARSE_URGENT | REPARSE_FORCE_CMAKE_RUN
                                     | REPARSE_FORCE_CONFIGURATION | REPARSE_CHECK_CONFIGURATION);
    return true;
}

void CMakeBuildSystem::clearCMakeCache()
{
    QTC_ASSERT(m_parameters.isValid(), return );
    QTC_ASSERT(!m_isHandlingError, return );

    stopParsingAndClearState();

    const FilePath cmakeCache = m_parameters.workDirectory / "CMakeCache.txt";
    const FilePath cmakeFiles = m_parameters.workDirectory / "CMakeFiles";

    if (cmakeCache.exists())
        Utils::FileUtils::removeRecursively(cmakeCache);
    if (cmakeFiles.exists())
        Utils::FileUtils::removeRecursively(cmakeFiles);
}

std::unique_ptr<CMakeProjectNode>
    CMakeBuildSystem::generateProjectTree(const QList<const FileNode *> &allFiles)
{
    QString errorMessage;
    auto root = m_reader.generateProjectTree(allFiles, errorMessage);
    checkAndReportError(errorMessage);
    return root;
}

void CMakeBuildSystem::combineScanAndParse()
{
    if (cmakeBuildConfiguration()->isActive()) {
        if (m_waitingForParse || m_waitingForScan)
            return;

        if (m_combinedScanAndParseResult) {
            updateProjectData();
            m_currentGuard.markAsSuccess();
        }
    }

    m_reader.resetData();

    m_currentGuard = {};

    emitBuildSystemUpdated();
}

void CMakeBuildSystem::checkAndReportError(QString &errorMessage)
{
    if (!errorMessage.isEmpty()) {
        cmakeBuildConfiguration()->setError(errorMessage);
        errorMessage.clear();
    }
}

void CMakeBuildSystem::updateProjectData()
{
    qCDebug(cmakeBuildSystemLog) << "Updating CMake project data";

    QTC_ASSERT(m_treeScanner.isFinished() && !m_reader.isParsing(), return );

    cmakeBuildConfiguration()->project()->setExtraProjectFiles(m_reader.projectFilesToWatch());

    CMakeConfig patchedConfig = cmakeBuildConfiguration()->configurationFromCMake();
    {
        CMakeConfigItem settingFileItem;
        settingFileItem.key = "ANDROID_DEPLOYMENT_SETTINGS_FILE";
        settingFileItem.value = cmakeBuildConfiguration()
                                    ->buildDirectory()
                                    .pathAppended("android_deployment_settings.json")
                                    .toString()
                                    .toUtf8();
        patchedConfig.append(settingFileItem);
    }
    {
        QSet<QString> res;
        QStringList apps;
        for (const auto &target : m_buildTargets) {
            if (target.targetType == CMakeProjectManager::DynamicLibraryType) {
                res.insert(target.executable.parentDir().toString());
                apps.push_back(target.executable.toUserOutput());
            }
            // ### shall we add also the ExecutableType ?
        }
        {
            CMakeConfigItem paths;
            paths.key = "ANDROID_SO_LIBS_PATHS";
            paths.values = Utils::toList(res);
            patchedConfig.append(paths);
        }

        apps.sort();
        {
            CMakeConfigItem appsPaths;
            appsPaths.key = "TARGETS_BUILD_PATH";
            appsPaths.values = apps;
            patchedConfig.append(appsPaths);
        }
    }

    Project *p = project();
    {
        auto newRoot = generateProjectTree(m_allFiles);
        if (newRoot) {
            setRootProjectNode(std::move(newRoot));
            if (p->rootProjectNode())
                p->setDisplayName(p->rootProjectNode()->displayName());

            for (const CMakeBuildTarget &bt : m_buildTargets) {
                const QString buildKey = bt.title;
                if (ProjectNode *node = p->findNodeForBuildKey(buildKey)) {
                    if (auto targetNode = dynamic_cast<CMakeTargetNode *>(node))
                        targetNode->setConfig(patchedConfig);
                }
            }
        }
    }

    {
        qDeleteAll(m_extraCompilers);
        m_extraCompilers = findExtraCompilers();
        CppTools::GeneratedCodeModelSupport::update(m_extraCompilers);
        qCDebug(cmakeBuildSystemLog) << "Extra compilers updated.";
    }

    QtSupport::CppKitInfo kitInfo(kit());
    QTC_ASSERT(kitInfo.isValid(), return );

    {
        QString errorMessage;
        RawProjectParts rpps = m_reader.createRawProjectParts(errorMessage);
        if (!errorMessage.isEmpty())
            cmakeBuildConfiguration()->setError(errorMessage);
        qCDebug(cmakeBuildSystemLog) << "Raw project parts created." << errorMessage;

        for (RawProjectPart &rpp : rpps) {
            rpp.setQtVersion(
                kitInfo.projectPartQtVersion); // TODO: Check if project actually uses Qt.
            if (kitInfo.cxxToolChain)
                rpp.setFlagsForCxx({kitInfo.cxxToolChain, rpp.flagsForCxx.commandLineFlags});
            if (kitInfo.cToolChain)
                rpp.setFlagsForC({kitInfo.cToolChain, rpp.flagsForC.commandLineFlags});
        }

        m_cppCodeModelUpdater->update({p, kitInfo, cmakeBuildConfiguration()->environment(), rpps});
    }
    {
        updateQmlJSCodeModel();
    }

    emit cmakeBuildConfiguration()->buildTypeChanged();

    qCDebug(cmakeBuildSystemLog) << "All CMake project data up to date.";
}

void CMakeBuildSystem::handleParsingSucceeded()
{
    if (!cmakeBuildConfiguration()->isActive()) {
        stopParsingAndClearState();
        return;
    }

    cmakeBuildConfiguration()->clearError();

    QString errorMessage;
    {
        m_buildTargets = m_reader.takeBuildTargets(errorMessage);
        checkAndReportError(errorMessage);
    }

    {
        CMakeConfig cmakeConfig = m_reader.takeParsedConfiguration(errorMessage);
        for (auto &ci : cmakeConfig)
            ci.inCMakeCache = true;
        cmakeBuildConfiguration()->setConfigurationFromCMake(cmakeConfig);
        checkAndReportError(errorMessage);
    }

    setApplicationTargets(appTargets());
    setDeploymentData(deploymentData());

    QTC_ASSERT(m_waitingForParse, return );
    m_waitingForParse = false;

    combineScanAndParse();
}

void CMakeBuildSystem::handleParsingFailed(const QString &msg)
{
    cmakeBuildConfiguration()->setError(msg);

    QString errorMessage;
    CMakeConfig cmakeConfig = m_reader.takeParsedConfiguration(errorMessage);
    for (auto &ci : cmakeConfig)
        ci.inCMakeCache = true;
    cmakeBuildConfiguration()->setConfigurationFromCMake(cmakeConfig);
    // ignore errorMessage here, we already got one.

    QTC_CHECK(m_waitingForParse);
    m_waitingForParse = false;
    m_combinedScanAndParseResult = false;

    combineScanAndParse();
}

void CMakeBuildSystem::wireUpConnections(const Project *p)
{
    if (p != project())
        return; // That's not us...

    disconnect(SessionManager::instance(), nullptr, this, nullptr);

    // At this point the entire project will be fully configured, so let's connect everything and
    // trigger an initial parser run

    // Kit changed:
    connect(KitManager::instance(), &KitManager::kitUpdated, this, [this](Kit *k) {
        if (k != kit())
            return; // not for us...
        // Build configuration has not changed, but Kit settings might have:
        // reparse and check the configuration, independent of whether the reader has changed
        qCDebug(cmakeBuildSystemLog) << "Requesting parse due to kit being updated";
        setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                     CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
    });

    // Became active/inactive:
    connect(project(), &Project::activeTargetChanged, this, [this](Target *t) {
        if (t == target()) {
            // Build configuration has switched:
            // * Check configuration if reader changes due to it not existing yet:-)
            // * run cmake without configuration arguments if the reader stays
            qCDebug(cmakeBuildSystemLog) << "Requesting parse due to active target changed";
            setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                         CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
        } else {
            stopParsingAndClearState();
        }
    });
    connect(target(), &Target::activeBuildConfigurationChanged, this, [this](BuildConfiguration *bc) {
        if (cmakeBuildConfiguration()->isActive()) {
            if (cmakeBuildConfiguration() == bc) {
                // Build configuration has switched:
                // * Check configuration if reader changes due to it not existing yet:-)
                // * run cmake without configuration arguments if the reader stays
                qCDebug(cmakeBuildSystemLog) << "Requesting parse due to active BC changed";
                setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                             CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
            } else {
                stopParsingAndClearState();
            }
        }
    });

    // BuildConfiguration changed:
    connect(cmakeBuildConfiguration(), &CMakeBuildConfiguration::environmentChanged, this, [this]() {
        if (cmakeBuildConfiguration()->isActive()) {
            // The environment on our BC has changed:
            // * Error out if the reader updates, cannot happen since all BCs share a target/kit.
            // * run cmake without configuration arguments if the reader stays
            qCDebug(cmakeBuildSystemLog) << "Requesting parse due to environment change";
            setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                         CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
        }
    });
    connect(cmakeBuildConfiguration(), &CMakeBuildConfiguration::buildDirectoryChanged, this, [this]() {
        if (cmakeBuildConfiguration()->isActive()) {
            // The build directory of our BC has changed:
            // * Error out if the reader updates, cannot happen since all BCs share a target/kit.
            // * run cmake without configuration arguments if the reader stays
            //   If no configuration exists, then the arguments will get added automatically by
            //   the reader.
            qCDebug(cmakeBuildSystemLog) << "Requesting parse due to build directory change";
            setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                         CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
        }
    });
    connect(cmakeBuildConfiguration(),
            &CMakeBuildConfiguration::configurationForCMakeChanged,
            this,
            [this]() {
                if (cmakeBuildConfiguration()->isActive()) {
                    // The CMake configuration has changed on our BC:
                    // * Error out if the reader updates, cannot happen since all BCs share a target/kit.
                    // * run cmake with configuration arguments if the reader stays
                    qCDebug(cmakeBuildSystemLog)
                        << "Requesting parse due to cmake configuration change";
                    setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                                 CMakeBuildSystem::REPARSE_FORCE_CONFIGURATION);
                }
            });

    connect(project(), &Project::projectFileIsDirty, this, [this]() {
        if (cmakeBuildConfiguration()->isActive() && !isParsing()) {
            const auto cmake = CMakeKitAspect::cmakeTool(cmakeBuildConfiguration()->target()->kit());
            if (cmake && cmake->isAutoRun()) {
                qCDebug(cmakeBuildSystemLog) << "Requesting parse due to dirty project file";
                setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                             CMakeBuildSystem::REPARSE_DEFAULT);
            }
        }
    });

    // Force initial parsing run:
    if (cmakeBuildConfiguration()->isActive())
        setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                     CMakeBuildSystem::REPARSE_CHECK_CONFIGURATION);
}

FilePath CMakeBuildSystem::workDirectory(const BuildDirParameters &parameters)
{
    const Utils::FilePath bdir = parameters.buildDirectory;
    const CMakeTool *cmake = parameters.cmakeTool();
    if (bdir.exists()) {
        m_buildDirToTempDir.erase(bdir);
        return bdir;
    }

    if (cmake && cmake->autoCreateBuildDirectory()) {
        if (!cmakeBuildConfiguration()->createBuildDirectory())
            handleParsingFailed(
                tr("Failed to create build directory \"%1\".").arg(bdir.toUserOutput()));
        return bdir;
    }

    auto tmpDirIt = m_buildDirToTempDir.find(bdir);
    if (tmpDirIt == m_buildDirToTempDir.end()) {
        auto ret = m_buildDirToTempDir.emplace(
            std::make_pair(bdir, std::make_unique<Utils::TemporaryDirectory>("qtc-cmake-XXXXXXXX")));
        QTC_ASSERT(ret.second, return bdir);
        tmpDirIt = ret.first;

        if (!tmpDirIt->second->isValid()) {
            handleParsingFailed(tr("Failed to create temporary directory \"%1\".")
                                    .arg(QDir::toNativeSeparators(tmpDirIt->second->path())));
            return bdir;
        }
    }
    return Utils::FilePath::fromString(tmpDirIt->second->path());
}

void CMakeBuildSystem::stopParsingAndClearState()
{
    qCDebug(cmakeBuildSystemLog) << "stopping parsing run!";
    m_reader.stop();
    m_reader.resetData();
}

void CMakeBuildSystem::becameDirty()
{
    qCDebug(cmakeBuildSystemLog) << "CMakeBuildSystem: becameDirty was triggered.";
    if (isParsing())
        return;

    const CMakeTool *tool = m_parameters.cmakeTool();
    if (!tool->isAutoRun())
        return;

    setParametersAndRequestParse(BuildDirParameters(cmakeBuildConfiguration()),
                                 REPARSE_CHECK_CONFIGURATION | REPARSE_SCAN);
}

void CMakeBuildSystem::updateReparseParameters(const int parameters)
{
    m_reparseParameters |= parameters;
}

int CMakeBuildSystem::takeReparseParameters()
{
    int result = m_reparseParameters;
    m_reparseParameters = REPARSE_DEFAULT;
    return result;
}

bool CMakeBuildSystem::hasConfigChanged()
{
    checkConfiguration();

    const QByteArray GENERATOR_KEY = "CMAKE_GENERATOR";
    const QByteArray EXTRA_GENERATOR_KEY = "CMAKE_EXTRA_GENERATOR";
    const QByteArray CMAKE_COMMAND_KEY = "CMAKE_COMMAND";
    const QByteArray CMAKE_C_COMPILER_KEY = "CMAKE_C_COMPILER";
    const QByteArray CMAKE_CXX_COMPILER_KEY = "CMAKE_CXX_COMPILER";

    const QByteArrayList criticalKeys = {GENERATOR_KEY,
                                         CMAKE_COMMAND_KEY,
                                         CMAKE_C_COMPILER_KEY,
                                         CMAKE_CXX_COMPILER_KEY};

    QString errorMessage;
    const CMakeConfig currentConfig = cmakeBuildConfiguration()->configurationFromCMake();
    if (!errorMessage.isEmpty())
        return false;

    const CMakeTool *tool = m_parameters.cmakeTool();
    QTC_ASSERT(tool,
               return false); // No cmake... we should not have ended up here in the first place
    const QString extraKitGenerator = m_parameters.extraGenerator;
    const QString mainKitGenerator = m_parameters.generator;
    CMakeConfig targetConfig = m_parameters.configuration;
    targetConfig.append(CMakeConfigItem(GENERATOR_KEY,
                                        CMakeConfigItem::INTERNAL,
                                        QByteArray(),
                                        mainKitGenerator.toUtf8()));
    if (!extraKitGenerator.isEmpty())
        targetConfig.append(CMakeConfigItem(EXTRA_GENERATOR_KEY,
                                            CMakeConfigItem::INTERNAL,
                                            QByteArray(),
                                            extraKitGenerator.toUtf8()));
    targetConfig.append(CMakeConfigItem(CMAKE_COMMAND_KEY,
                                        CMakeConfigItem::INTERNAL,
                                        QByteArray(),
                                        tool->cmakeExecutable().toUserOutput().toUtf8()));
    Utils::sort(targetConfig, CMakeConfigItem::sortOperator());

    bool mustReparse = false;
    auto ccit = currentConfig.constBegin();
    auto kcit = targetConfig.constBegin();

    while (ccit != currentConfig.constEnd() && kcit != targetConfig.constEnd()) {
        if (ccit->key == kcit->key) {
            if (ccit->value != kcit->value) {
                if (criticalKeys.contains(kcit->key)) {
                    clearCMakeCache();
                    return false; // no need to trigger a new reader, clearCache will do that
                }
                mustReparse = true;
            }
            ++ccit;
            ++kcit;
        } else {
            if (ccit->key < kcit->key) {
                ++ccit;
            } else {
                ++kcit;
                mustReparse = true;
            }
        }
    }

    // If we have keys that do not exist yet, then reparse.
    //
    // The critical keys *must* be set in cmake configuration, so those were already
    // handled above.
    return mustReparse || kcit != targetConfig.constEnd();
}

bool CMakeBuildSystem::checkConfiguration()
{
    if (m_parameters.workDirectory
        != m_parameters.buildDirectory) // always throw away changes in the tmpdir!
        return false;

    const CMakeConfig cache = cmakeBuildConfiguration()->configurationFromCMake();
    if (cache.isEmpty())
        return false; // No cache file yet.

    CMakeConfig newConfig;
    QHash<QString, QPair<QString, QString>> changedKeys;
    foreach (const CMakeConfigItem &projectItem, m_parameters.configuration) {
        const QString projectKey = QString::fromUtf8(projectItem.key);
        const QString projectValue = projectItem.expandedValue(m_parameters.expander);
        const CMakeConfigItem &cmakeItem = Utils::findOrDefault(cache,
                                                                [&projectItem](
                                                                    const CMakeConfigItem &i) {
                                                                    return i.key == projectItem.key;
                                                                });
        const QString iCacheValue = QString::fromUtf8(cmakeItem.value);
        if (cmakeItem.isNull()) {
            changedKeys.insert(projectKey, qMakePair(tr("<removed>"), projectValue));
        } else if (iCacheValue != projectValue) {
            changedKeys.insert(projectKey, qMakePair(iCacheValue, projectValue));
            newConfig.append(cmakeItem);
        } else {
            newConfig.append(projectItem);
        }
    }

    if (!changedKeys.isEmpty()) {
        QStringList keyList = changedKeys.keys();
        Utils::sort(keyList);
        QString table = QString::fromLatin1("<table><tr><th>%1</th><th>%2</th><th>%3</th></tr>")
                            .arg(tr("Key"))
                            .arg(tr("%1 Project").arg(Core::Constants::IDE_DISPLAY_NAME))
                            .arg(tr("Changed value"));
        foreach (const QString &k, keyList) {
            const QPair<QString, QString> data = changedKeys.value(k);
            table += QString::fromLatin1("\n<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
                         .arg(k)
                         .arg(data.second.toHtmlEscaped())
                         .arg(data.first.toHtmlEscaped());
        }
        table += QLatin1String("\n</table>");

        QPointer<QMessageBox> box = new QMessageBox(Core::ICore::mainWindow());
        box->setText(tr("The project has been changed outside of %1.")
                         .arg(Core::Constants::IDE_DISPLAY_NAME));
        box->setInformativeText(table);
        auto *defaultButton = box->addButton(tr("Discard External Changes"),
                                             QMessageBox::RejectRole);
        auto *applyButton = box->addButton(tr("Adapt %1 Project to Changes")
                                               .arg(Core::Constants::IDE_DISPLAY_NAME),
                                           QMessageBox::ApplyRole);
        box->setDefaultButton(defaultButton);

        box->exec();
        if (box->clickedButton() == applyButton) {
            m_parameters.configuration = newConfig;
            QSignalBlocker blocker(buildConfiguration());
            cmakeBuildConfiguration()->setConfigurationForCMake(newConfig);
            return false;
        } else if (box->clickedButton() == defaultButton)
            return true;
    }
    return false;
}

CMakeBuildConfiguration *CMakeBuildSystem::cmakeBuildConfiguration() const
{
    return static_cast<CMakeBuildConfiguration *>(BuildSystem::buildConfiguration());
}

static Utils::FilePaths librarySearchPaths(const CMakeBuildSystem *bs, const QString &buildKey)
{
    const CMakeBuildTarget cmakeBuildTarget
        = Utils::findOrDefault(bs->buildTargets(), Utils::equal(&CMakeBuildTarget::title, buildKey));

    return cmakeBuildTarget.libraryDirectories;
}

const QList<BuildTargetInfo> CMakeBuildSystem::appTargets() const
{
    QList<BuildTargetInfo> appTargetList;
    const bool forAndroid = DeviceTypeKitAspect::deviceTypeId(kit())
                            == Android::Constants::ANDROID_DEVICE_TYPE;
    for (const CMakeBuildTarget &ct : m_buildTargets) {
        if (ct.targetType == UtilityType)
            continue;

        if (ct.targetType == ExecutableType || (forAndroid && ct.targetType == DynamicLibraryType)) {
            const QString buildKey = ct.title;

            BuildTargetInfo bti;
            bti.displayName = ct.title;
            bti.targetFilePath = ct.executable;
            bti.projectFilePath = ct.sourceDirectory.stringAppended("/");
            bti.workingDirectory = ct.workingDirectory;
            bti.buildKey = buildKey;
            bti.usesTerminal = !ct.linksToQtGui;

            // Workaround for QTCREATORBUG-19354:
            bti.runEnvModifier = [this, buildKey](Environment &env, bool enabled) {
                if (enabled) {
                    const Utils::FilePaths paths = librarySearchPaths(this, buildKey);
                    env.prependOrSetLibrarySearchPaths(Utils::transform(paths, &FilePath::toString));
                }
            };

            appTargetList.append(bti);
        }
    }

    return appTargetList;
}

QStringList CMakeBuildSystem::buildTargetTitles() const
{
    return transform(m_buildTargets, &CMakeBuildTarget::title);
}

const QList<CMakeBuildTarget> &CMakeBuildSystem::buildTargets() const
{
    return m_buildTargets;
}

CMakeConfig CMakeBuildSystem::parseCMakeCacheDotTxt(const Utils::FilePath &cacheFile,
                                                    QString *errorMessage)
{
    if (!cacheFile.exists()) {
        if (errorMessage)
            *errorMessage = tr("CMakeCache.txt file not found.");
        return {};
    }
    CMakeConfig result = CMakeConfigItem::itemsFromFile(cacheFile, errorMessage);
    if (!errorMessage->isEmpty())
        return {};
    return result;
}

DeploymentData CMakeBuildSystem::deploymentData() const
{
    DeploymentData result;

    QDir sourceDir = project()->projectDirectory().toString();
    QDir buildDir = cmakeBuildConfiguration()->buildDirectory().toString();

    QString deploymentPrefix;
    QString deploymentFilePath = sourceDir.filePath("QtCreatorDeployment.txt");

    bool hasDeploymentFile = QFileInfo::exists(deploymentFilePath);
    if (!hasDeploymentFile) {
        deploymentFilePath = buildDir.filePath("QtCreatorDeployment.txt");
        hasDeploymentFile = QFileInfo::exists(deploymentFilePath);
    }
    if (!hasDeploymentFile)
        return result;

    deploymentPrefix = result.addFilesFromDeploymentFile(deploymentFilePath,
                                                         sourceDir.absolutePath());
    for (const CMakeBuildTarget &ct : m_buildTargets) {
        if (ct.targetType == ExecutableType || ct.targetType == DynamicLibraryType) {
            if (!ct.executable.isEmpty()
                    && result.deployableForLocalFile(ct.executable).localFilePath() != ct.executable) {
                result.addFile(ct.executable.toString(),
                               deploymentPrefix + buildDir.relativeFilePath(ct.executable.toFileInfo().dir().path()),
                               DeployableFile::TypeExecutable);
            }
        }
    }

    return result;
}

QList<ProjectExplorer::ExtraCompiler *> CMakeBuildSystem::findExtraCompilers()
{
    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: start.";

    QList<ProjectExplorer::ExtraCompiler *> extraCompilers;
    const QList<ExtraCompilerFactory *> factories = ExtraCompilerFactory::extraCompilerFactories();

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got factories.";

    const QSet<QString> fileExtensions = Utils::transform<QSet>(factories,
                                                                &ExtraCompilerFactory::sourceTag);

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got file extensions:"
                                 << fileExtensions;

    // Find all files generated by any of the extra compilers, in a rather crude way.
    Project *p = project();
    const FilePaths fileList = p->files([&fileExtensions, p](const Node *n) {
        if (!p->SourceFiles(n))
            return false;
        const QString fp = n->filePath().toString();
        const int pos = fp.lastIndexOf('.');
        return pos >= 0 && fileExtensions.contains(fp.mid(pos + 1));
    });

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got list of files to check.";

    // Generate the necessary information:
    for (const FilePath &file : fileList) {
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers: Processing" << file.toUserOutput();
        ExtraCompilerFactory *factory = Utils::findOrDefault(factories,
                                                             [&file](const ExtraCompilerFactory *f) {
                                                                 return file.endsWith(
                                                                     '.' + f->sourceTag());
                                                             });
        QTC_ASSERT(factory, continue);

        QStringList generated = filesGeneratedFrom(file.toString());
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers:     generated files:" << generated;
        if (generated.isEmpty())
            continue;

        const FilePaths fileNames = transform(generated, [](const QString &s) {
            return FilePath::fromString(s);
        });
        extraCompilers.append(factory->create(p, file, fileNames));
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers:     done with" << file.toUserOutput();
    }

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: done.";

    return extraCompilers;
}

void CMakeBuildSystem::updateQmlJSCodeModel()
{
    QmlJS::ModelManagerInterface *modelManager = QmlJS::ModelManagerInterface::instance();

    if (!modelManager)
        return;

    Project *p = project();
    QmlJS::ModelManagerInterface::ProjectInfo projectInfo = modelManager
                                                                ->defaultProjectInfoForProject(p);

    projectInfo.importPaths.clear();

    const CMakeConfig &cm = cmakeBuildConfiguration()->configurationFromCMake();
    const QString cmakeImports = QString::fromUtf8(CMakeConfigItem::valueOf("QML_IMPORT_PATH", cm));

    foreach (const QString &cmakeImport, CMakeConfigItem::cmakeSplitValue(cmakeImports))
        projectInfo.importPaths.maybeInsert(FilePath::fromString(cmakeImport), QmlJS::Dialect::Qml);

    project()->setProjectLanguage(ProjectExplorer::Constants::QMLJS_LANGUAGE_ID,
                                  !projectInfo.sourceFiles.isEmpty());
    modelManager->updateProjectInfo(projectInfo, p);
}

} // namespace Internal
} // namespace CMakeProjectManager
