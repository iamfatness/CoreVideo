#include "zoom-output-dialog.h"
#include "zoom-output-manager.h"
#include "zoom-participants.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

enum OutputColumns {
    ColumnName = 0,
    ColumnAssignment,
    ColumnAudio,
    ColumnIsolate,
    ColumnCount
};

static QString participant_label(const ParticipantInfo &p)
{
    QString label = p.display_name.empty()
        ? QString("ID %1").arg(p.user_id)
        : QString::fromStdString(p.display_name);
    if (p.is_talking) label += " *";
    if (p.has_video) label += " [video]";
    return label;
}

ZoomOutputDialog::ZoomOutputDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Zoom Output Manager");
    resize(760, 420);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter participants");
    connect(m_filter, &QLineEdit::textChanged, this,
            [this]() { refresh_participants(); });

    m_participant_table = new QTableWidget(this);
    m_participant_table->setColumnCount(5);
    m_participant_table->setHorizontalHeaderLabels({
        "Participant", "ID", "Video", "Audio", "Talking"
    });
    m_participant_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_participant_table->verticalHeader()->setVisible(false);
    m_participant_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_participant_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        "Output", "Assignment", "Audio", "Isolated audio"
    });
    m_table->horizontalHeader()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColumnAssignment, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                         QDialogButtonBox::Close, this);
    auto *refresh_button = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);

    connect(refresh_button, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, [this]() { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Participants", this));
    layout->addWidget(m_filter);
    layout->addWidget(m_participant_table);
    layout->addWidget(new QLabel("Outputs", this));
    layout->addWidget(m_table);
    layout->addWidget(buttons);

    ZoomParticipants::instance().add_roster_callback(this, [this]() {
        QMetaObject::invokeMethod(this, [this]() { refresh(); }, Qt::QueuedConnection);
    });
    refresh();
}

ZoomOutputDialog::~ZoomOutputDialog()
{
    ZoomParticipants::instance().remove_roster_callback(this);
}

void ZoomOutputDialog::refresh()
{
    refresh_participants();

    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto roster = ZoomParticipants::instance().roster();

    m_table->setRowCount(static_cast<int>(outputs.size()));
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        const auto &output = outputs[row];

        auto *name_item = new QTableWidgetItem(QString::fromStdString(output.source_name));
        name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
        name_item->setData(Qt::UserRole, QString::fromStdString(output.source_name));
        m_table->setItem(row, ColumnName, name_item);

        auto *assignment = new QComboBox(m_table);
        assignment->addItem("Active speaker", "active");
        assignment->addItem("None", "user:0");
        for (const auto &p : roster)
            assignment->addItem(participant_label(p), QString("user:%1").arg(p.user_id));
        const QString current_assignment = output.active_speaker
            ? "active"
            : QString("user:%1").arg(output.participant_id);
        const int assignment_index = assignment->findData(current_assignment);
        if (assignment_index >= 0) assignment->setCurrentIndex(assignment_index);
        m_table->setCellWidget(row, ColumnAssignment, assignment);

        auto *audio = new QComboBox(m_table);
        audio->addItem("Mono", static_cast<int>(AudioChannelMode::Mono));
        audio->addItem("Stereo", static_cast<int>(AudioChannelMode::Stereo));
        audio->setCurrentIndex(output.audio_mode == AudioChannelMode::Stereo ? 1 : 0);
        m_table->setCellWidget(row, ColumnAudio, audio);

        auto *isolate = new QCheckBox(m_table);
        isolate->setChecked(output.isolate_audio);
        isolate->setToolTip("Use the assigned participant's isolated audio instead of the meeting mix.");
        m_table->setCellWidget(row, ColumnIsolate, isolate);
    }
}

void ZoomOutputDialog::refresh_participants()
{
    const QString filter = m_filter ? m_filter->text().trimmed() : QString{};
    const auto roster = ZoomParticipants::instance().roster();

    int row = 0;
    m_participant_table->setRowCount(0);
    for (const auto &p : roster) {
        const QString name = p.display_name.empty()
            ? QString("ID %1").arg(p.user_id)
            : QString::fromStdString(p.display_name);
        const QString id = QString::number(p.user_id);
        if (!filter.isEmpty() &&
            !name.contains(filter, Qt::CaseInsensitive) &&
            !id.contains(filter)) {
            continue;
        }

        m_participant_table->insertRow(row);
        m_participant_table->setItem(row, 0, new QTableWidgetItem(name));
        m_participant_table->setItem(row, 1, new QTableWidgetItem(id));
        m_participant_table->setItem(row, 2,
            new QTableWidgetItem(p.has_video ? "On" : "Off"));
        m_participant_table->setItem(row, 3,
            new QTableWidgetItem(p.is_muted ? "Muted" : "Open"));
        m_participant_table->setItem(row, 4,
            new QTableWidgetItem(p.is_talking ? "Yes" : ""));
        ++row;
    }
}

void ZoomOutputDialog::apply()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *name_item = m_table->item(row, ColumnName);
        auto *assignment = qobject_cast<QComboBox *>(
            m_table->cellWidget(row, ColumnAssignment));
        auto *audio = qobject_cast<QComboBox *>(
            m_table->cellWidget(row, ColumnAudio));
        auto *isolate = qobject_cast<QCheckBox *>(
            m_table->cellWidget(row, ColumnIsolate));
        if (!name_item || !assignment || !audio || !isolate) continue;

        const std::string source_name =
            name_item->data(Qt::UserRole).toString().toStdString();
        const QString assignment_data = assignment->currentData().toString();
        const bool active_speaker = assignment_data == "active";
        uint32_t participant_id = 0;
        if (assignment_data.startsWith("user:"))
            participant_id = assignment_data.mid(5).toUInt();
        const auto audio_mode = static_cast<AudioChannelMode>(
            audio->currentData().toInt());

        ZoomOutputManager::instance().configure_output(
            source_name, participant_id, active_speaker,
            isolate->isChecked(), audio_mode);
    }
    refresh();
}
