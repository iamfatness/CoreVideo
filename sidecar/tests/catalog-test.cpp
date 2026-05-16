#include "layout-template.h"
#include "look.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>

static int fail(const QString &message)
{
    QTextStream(stderr) << message << '\n';
    return 1;
}

static bool loadJsonObject(const QString &path, QJsonObject *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("Could not open %1").arg(path));
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(QStringLiteral("Invalid JSON in %1: %2").arg(path, err.errorString()));
        return false;
    }

    *out = doc.object();
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 2)
        return fail(QStringLiteral("Usage: CoreVideoSidecarCatalogTest <sidecar-data-dir>"));

    const QDir dataDir(app.arguments().at(1));
    const QDir templateDir(dataDir.filePath(QStringLiteral("templates")));
    const QDir lookDir(dataDir.filePath(QStringLiteral("looks")));
    if (!templateDir.exists() || !lookDir.exists())
        return fail(QStringLiteral("Missing catalog directories under %1").arg(dataDir.path()));

    QHash<QString, LayoutTemplate> templates;
    for (const QString &name : templateDir.entryList(QStringList() << "*.json", QDir::Files, QDir::Name)) {
        QJsonObject obj;
        if (!loadJsonObject(templateDir.filePath(name), &obj)) return 1;

        const LayoutTemplate tmpl = LayoutTemplate::fromJson(obj);
        if (!tmpl.isValid())
            return fail(QStringLiteral("Invalid template %1").arg(name));

        QSet<int> indexes;
        for (const TemplateSlot &slot : tmpl.slotList) {
            if (indexes.contains(slot.index))
                return fail(QStringLiteral("Duplicate slot index %1 in %2").arg(slot.index).arg(name));
            indexes.insert(slot.index);
            if (slot.x < 0.0 || slot.y < 0.0 || slot.width <= 0.0 || slot.height <= 0.0
                || slot.x + slot.width > 1.0001 || slot.y + slot.height > 1.0001) {
                return fail(QStringLiteral("Out-of-bounds slot %1 in %2").arg(slot.index).arg(name));
            }
        }

        templates.insert(tmpl.id, tmpl);
    }

    const QHash<QString, int> requiredLooks = {
        {QStringLiteral("one-up-clean"), 1},
        {QStringLiteral("two-up-clean"), 2},
        {QStringLiteral("three-up-feature"), 3},
        {QStringLiteral("four-up-clean"), 4},
        {QStringLiteral("five-up-feature"), 5},
        {QStringLiteral("six-up-grid"), 6},
        {QStringLiteral("seven-up-feature"), 7},
        {QStringLiteral("eight-up-grid"), 8},
        {QStringLiteral("speaker-screenshare"), 2},
    };
    QSet<QString> seenLooks;

    for (const QString &name : lookDir.entryList(QStringList() << "*.json", QDir::Files, QDir::Name)) {
        QJsonObject obj;
        if (!loadJsonObject(lookDir.filePath(name), &obj)) return 1;

        const Look look = Look::fromJson(obj);
        if (look.id.isEmpty() || look.name.isEmpty() || look.templateId.isEmpty())
            return fail(QStringLiteral("Invalid look metadata in %1").arg(name));
        if (!templates.contains(look.templateId))
            return fail(QStringLiteral("Look %1 references missing template %2").arg(look.id, look.templateId));

        seenLooks.insert(look.id);
        if (requiredLooks.contains(look.id)) {
            const int expectedSlots = requiredLooks.value(look.id);
            const int actualSlots = templates.value(look.templateId).slotList.size();
            if (actualSlots != expectedSlots) {
                return fail(QStringLiteral("Look %1 expected %2 slots, got %3")
                                .arg(look.id).arg(expectedSlots).arg(actualSlots));
            }
        }
    }

    for (auto it = requiredLooks.constBegin(); it != requiredLooks.constEnd(); ++it) {
        if (!seenLooks.contains(it.key()))
            return fail(QStringLiteral("Missing required look %1").arg(it.key()));
    }

    const LayoutTemplate shareTemplate = templates.value(QStringLiteral("speaker-screenshare"));
    if (!shareTemplate.isValid())
        return fail(QStringLiteral("Missing speaker-screenshare template"));
    bool hasSpeakerPip = false;
    bool hasFullScreenShare = false;
    for (const TemplateSlot &slot : shareTemplate.slotList) {
        if (slot.index == 0 && slot.x > 0.5 && slot.y > 0.5 && slot.width < 0.5 && slot.height < 0.5)
            hasSpeakerPip = true;
        if (slot.index == 1 && slot.isMain && slot.x == 0.0 && slot.y == 0.0
            && slot.width == 1.0 && slot.height == 1.0) {
            hasFullScreenShare = true;
        }
    }
    if (!hasSpeakerPip || !hasFullScreenShare)
        return fail(QStringLiteral("speaker-screenshare must map slot 0 to speaker PiP and slot 1 to full-screen share"));

    return 0;
}
