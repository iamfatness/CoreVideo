#include "zoom-diagnostics-dialog.h"
#include "cv-style.h"
#include "obs-zoom-version.h"
#include "zoom-engine-client.h"
#include "zoom-iso-recorder.h"
#include "zoom-output-manager.h"
#include "zoom-settings.h"
#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <unordered_map>

enum OutputDiagnosticColumns {
    DiagOutput = 0,
    DiagParticipant,
    DiagAssignment,
    DiagRequested,
    DiagNegotiated,
    DiagObserved,
    DiagFps,
    DiagAge,
    DiagRetries,
    DiagSdk,
    DiagState,
    DiagOutputColumnCount
};

enum EventDiagnosticColumns {
    EventTime = 0,
    EventStage,
    EventSource,
    EventParticipant,
    EventMessage,
    EventColumnCount
};

static QString state_text(MeetingState state)
{
    switch (state) {
    case MeetingState::Idle: return QStringLiteral("Idle");
    case MeetingState::Joining: return QStringLiteral("Joining");
    case MeetingState::InMeeting: return QStringLiteral("In meeting");
    case MeetingState::Leaving: return QStringLiteral("Leaving");
    case MeetingState::Recovering: return QStringLiteral("Recovering");
    case MeetingState::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

static QString assignment_text(const ZoomOutputInfo &output)
{
    switch (output.assignment) {
    case AssignmentMode::ActiveSpeaker: return QStringLiteral("Active speaker");
    case AssignmentMode::SpotlightIndex:
        return QString("Spotlight %1").arg(output.spotlight_slot);
    case AssignmentMode::ScreenShare: return QStringLiteral("Screen share");
    case AssignmentMode::Participant:
    default: return QStringLiteral("Participant");
    }
}

static QString resolution_text(VideoResolution resolution)
{
    switch (resolution) {
    case VideoResolution::P360: return QStringLiteral("640x360");
    case VideoResolution::P1080: return QStringLiteral("1920x1080");
    case VideoResolution::P720:
    default: return QStringLiteral("1280x720");
    }
}

static QString observed_text(const ZoomOutputInfo &output)
{
    if (output.observed_width == 0 || output.observed_height == 0)
        return QStringLiteral("No signal");
    return QString("%1x%2").arg(output.observed_width).arg(output.observed_height);
}

static QString negotiated_text(const ZoomOutputInfo &output)
{
    if (output.negotiated_resolution < 0)
        return QStringLiteral("-");
    return resolution_text(static_cast<VideoResolution>(output.negotiated_resolution));
}

static QString sdk_text(const ZoomOutputInfo &output)
{
    QStringList parts;
    if (output.last_video_subscribe_code >= 0) {
        parts << QString("subscribe %1")
            .arg(output.last_video_subscribe_code == 0
                 ? QStringLiteral("ok")
                 : QString::number(output.last_video_subscribe_code));
    }
    if (output.last_set_resolution_code >= 0)
        parts << QString("setRes %1").arg(output.last_set_resolution_code);
    if (output.last_raw_status >= 0)
        parts << QString("raw %1").arg(output.last_raw_status);
    if (output.subscription_downgraded)
        parts << QStringLiteral("downgraded");
    if (!output.last_quality_stage.empty())
        parts << QString::fromStdString(output.last_quality_stage);
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(QStringLiteral(" / "));
}

static QString retry_text(const ZoomOutputInfo &output)
{
    QStringList parts;
    if (output.stale_recovery_attempts > 0)
        parts << QString("stale %1").arg(output.stale_recovery_attempts);
    if (output.quality_upgrade_attempts > 0)
        parts << QString("quality %1").arg(output.quality_upgrade_attempts);
    if (output.stale_recovery_cooldown_ms > 0)
        parts << QString("stale cd %1s").arg(output.stale_recovery_cooldown_ms / 1000);
    if (output.quality_upgrade_cooldown_ms > 0)
        parts << QString("quality cd %1s").arg(output.quality_upgrade_cooldown_ms / 1000);
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(QStringLiteral(" / "));
}

static QString signal_state_text(const ZoomOutputInfo &output)
{
    return QString::fromUtf8(output_health_reason_label(output.health_reason));
}

static QTableWidgetItem *readonly_item(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

struct TableViewState {
    int horizontal_scroll = 0;
    int vertical_scroll = 0;
    int current_row = -1;
    int current_column = -1;
    int selected_row = -1;
    int selected_column = -1;
    QString current_key;
    QString selected_key;
};

static QString table_row_key(const QTableWidget *table, int row,
                             const QVector<int> &columns)
{
    if (!table || row < 0 || row >= table->rowCount())
        return {};

    QStringList parts;
    for (int column : columns) {
        if (column < 0 || column >= table->columnCount())
            continue;
        const auto *item = table->item(row, column);
        parts << (item ? item->text() : QString{});
    }
    return parts.join(QLatin1Char('\x1f'));
}

static TableViewState capture_table_view_state(const QTableWidget *table,
                                               const QVector<int> &key_columns)
{
    TableViewState state;
    if (!table)
        return state;

    state.horizontal_scroll = table->horizontalScrollBar()->value();
    state.vertical_scroll = table->verticalScrollBar()->value();
    state.current_row = table->currentRow();
    state.current_column = table->currentColumn();
    state.current_key = table_row_key(table, state.current_row, key_columns);
    const QList<QTableWidgetItem *> selected = table->selectedItems();
    if (!selected.isEmpty()) {
        state.selected_row = selected.first()->row();
        state.selected_column = selected.first()->column();
        state.selected_key =
            table_row_key(table, state.selected_row, key_columns);
    }
    return state;
}

static int bounded_table_index(int value, int count)
{
    if (value < 0 || count <= 0)
        return -1;
    return std::min(value, count - 1);
}

static int find_table_row_by_key(const QTableWidget *table,
                                 const QVector<int> &key_columns,
                                 const QString &key)
{
    if (!table || key.isEmpty())
        return -1;

    for (int row = 0; row < table->rowCount(); ++row) {
        if (table_row_key(table, row, key_columns) == key)
            return row;
    }
    return -1;
}

static void restore_table_view_state(QTableWidget *table,
                                     const TableViewState &state,
                                     const QVector<int> &key_columns)
{
    if (!table)
        return;

    int current_row = find_table_row_by_key(table, key_columns,
                                            state.current_key);
    if (current_row < 0)
        current_row = bounded_table_index(state.current_row,
                                          table->rowCount());
    const int current_column = bounded_table_index(state.current_column,
                                                  table->columnCount());
    int selected_row = find_table_row_by_key(table, key_columns,
                                             state.selected_key);
    if (selected_row < 0)
        selected_row = bounded_table_index(state.selected_row,
                                           table->rowCount());
    const int selected_column = bounded_table_index(state.selected_column,
                                                   table->columnCount());

    table->clearSelection();
    if (current_row >= 0 && current_column >= 0)
        table->setCurrentCell(current_row, current_column,
                              QItemSelectionModel::NoUpdate);
    else
        table->setCurrentCell(-1, -1);

    if (selected_row >= 0 && selected_column >= 0) {
        if (auto *item = table->item(selected_row, selected_column))
            item->setSelected(true);
    }

    table->horizontalScrollBar()->setValue(state.horizontal_scroll);
    table->verticalScrollBar()->setValue(state.vertical_scroll);
}

static QString diagnostics_root_dir()
{
    const QStringList starts = {
        QDir::currentPath(),
        QCoreApplication::applicationDirPath()
    };
    for (const QString &start : starts) {
        QDir dir(start);
        for (int depth = 0; depth < 6; ++depth) {
            if (dir.exists(QStringLiteral("artifacts")))
                return dir.absoluteFilePath(
                    QStringLiteral("artifacts/support-bundles"));
            if (!dir.cdUp())
                break;
        }
    }

    char path[512] = {};
    if (os_get_config_path(path, sizeof(path),
            "obs-studio/plugin_config/obs-zoom-plugin/support-bundles") >= 0) {
        return QString::fromUtf8(path);
    }

    const QString docs = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    return QDir(base).absoluteFilePath(
        QStringLiteral("CoreVideo Support Bundles"));
}

static QString latest_obs_log_path()
{
#if defined(_WIN32)
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (appdata.isEmpty())
        return {};
    QDir logs(QDir(appdata).absoluteFilePath("obs-studio/logs"));
#elif defined(__APPLE__)
    QDir logs(QDir::home().absoluteFilePath(
        "Library/Application Support/obs-studio/logs"));
#else
    QDir logs(QDir::home().absoluteFilePath(".config/obs-studio/logs"));
#endif
    const QFileInfoList files = logs.entryInfoList(
        QStringList() << "*.txt",
        QDir::Files,
        QDir::Time);
    return files.isEmpty() ? QString{} : files.first().absoluteFilePath();
}

static QString assignment_id(const ZoomOutputInfo &output)
{
    switch (output.assignment) {
    case AssignmentMode::ActiveSpeaker: return QStringLiteral("active_speaker");
    case AssignmentMode::SpotlightIndex: return QStringLiteral("spotlight");
    case AssignmentMode::ScreenShare: return QStringLiteral("screen_share");
    case AssignmentMode::Participant:
    default: return QStringLiteral("participant");
    }
}

static QString resolution_id(VideoResolution resolution)
{
    switch (resolution) {
    case VideoResolution::P360: return QStringLiteral("360p");
    case VideoResolution::P1080: return QStringLiteral("1080p");
    case VideoResolution::P720:
    default: return QStringLiteral("720p");
    }
}

static QString hw_accel_mode_id(HwAccelMode mode)
{
    switch (mode) {
    case HwAccelMode::Auto: return QStringLiteral("auto");
    case HwAccelMode::Cuda: return QStringLiteral("cuda");
    case HwAccelMode::Vaapi: return QStringLiteral("vaapi");
    case HwAccelMode::VideoToolbox: return QStringLiteral("videotoolbox");
    case HwAccelMode::Qsv: return QStringLiteral("qsv");
    case HwAccelMode::None:
    default: return QStringLiteral("none");
    }
}

static QString redacted_value(const std::string &value)
{
    if (value.empty())
        return QStringLiteral("");
    if (value.size() <= 4)
        return QStringLiteral("[redacted len=%1]").arg(value.size());
    return QStringLiteral("[redacted len=%1 tail=%2]")
        .arg(value.size())
        .arg(QString::fromStdString(value.substr(value.size() - 4)));
}

static QString redacted_presence(const std::string &value)
{
    return value.empty()
        ? QStringLiteral("")
        : QStringLiteral("[redacted set len=%1]").arg(value.size());
}

static QString redact_support_text(QString text)
{
    const QVector<QPair<QRegularExpression, QString>> patterns = {
        {QRegularExpression(R"rx((Authorization:\s*(?:Bearer|Basic)\s+)[^\s\r\n]+)rx",
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1[redacted]")},
        {QRegularExpression(R"rx(((?:access_token|refresh_token|id_token|zak|jwt|token|code|client_secret|passcode|pwd)=)[^&\s\r\n]+)rx",
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1[redacted]")},
        {QRegularExpression(R"rx(("(?:access_token|refresh_token|id_token|zak|jwt|token|code|client_secret|passcode|pwd)"\s*:\s*")[^"]+("))rx",
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1[redacted]\\2")},
        {QRegularExpression(R"rx(((?:access_token|refresh_token|id_token|zak|jwt|token|code|client_secret|passcode|pwd)\s*[:=]\s*)[A-Za-z0-9._~+/=-]{12,})rx",
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1[redacted]")},
    };
    for (const auto &pattern : patterns)
        text.replace(pattern.first, pattern.second);
    return text;
}

static QString redacted_obs_log_excerpt(const QString &path, QStringList &warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        warnings << QStringLiteral("Could not read OBS log from %1").arg(path);
        return {};
    }

    QStringList lines;
    QTextStream in(&file);
    while (!in.atEnd()) {
        lines << in.readLine();
        if (lines.size() > 500)
            lines.removeFirst();
    }
    return redact_support_text(lines.join(QLatin1Char('\n'))) + QLatin1Char('\n');
}

static QJsonObject reconnect_policy_json(const ZoomReconnectPolicy &policy)
{
    QJsonObject obj;
    obj["enabled"] = policy.enabled;
    obj["max_attempts"] = policy.max_attempts;
    obj["base_delay_ms"] = policy.base_delay_ms;
    obj["max_delay_ms"] = policy.max_delay_ms;
    obj["on_engine_crash"] = policy.on_engine_crash;
    obj["on_disconnect"] = policy.on_disconnect;
    obj["on_auth_fail"] = policy.on_auth_fail;
    return obj;
}

static QJsonObject settings_json(const ZoomPluginSettings &settings)
{
    QJsonObject obj;
    obj["meeting_sdk_auth_mode"] =
        QString::fromStdString(settings.meeting_sdk_auth_mode);
    obj["sdk_key"] = redacted_value(settings.sdk_key);
    obj["sdk_secret"] = redacted_presence(settings.sdk_secret);
    obj["jwt_token"] = redacted_presence(settings.jwt_token);
    obj["meeting_sdk_public_app_key"] =
        redacted_value(settings.sdk_public_app_key);
    obj["resolved_meeting_sdk_public_app_key"] =
        redacted_value(settings.resolved_meeting_sdk_public_app_key());
    obj["oauth_client_id"] = redacted_value(settings.oauth_client_id);
    obj["oauth_authorization_url"] =
        QString::fromStdString(settings.oauth_authorization_url);
    obj["oauth_redirect_uri"] =
        QString::fromStdString(settings.oauth_redirect_uri);
    obj["oauth_scopes"] = QString::fromStdString(settings.oauth_scopes);
    obj["oauth_access_token"] =
        redacted_presence(settings.oauth_access_token);
    obj["oauth_refresh_token"] =
        redacted_presence(settings.oauth_refresh_token);
    obj["oauth_expires_at"] = static_cast<double>(settings.oauth_expires_at);
    obj["control_server_port"] = settings.control_server_port;
    obj["osc_server_port"] = settings.osc_server_port;
    obj["control_token"] = redacted_presence(settings.control_token);
    obj["hw_accel_mode"] = hw_accel_mode_id(settings.hw_accel_mode);
    obj["reconnect_policy"] =
        reconnect_policy_json(settings.reconnect_policy);
    obj["last_meeting_id"] = redacted_value(settings.last_meeting_id);
    obj["last_display_name_set"] = !settings.last_display_name.empty();
    obj["last_was_webinar"] = settings.last_was_webinar;
    obj["iso_output_dir"] = QString::fromStdString(settings.iso_output_dir);
    obj["iso_ffmpeg_path"] = QString::fromStdString(settings.iso_ffmpeg_path);
    obj["iso_video_encoder"] =
        QString::fromStdString(settings.iso_video_encoder);
    obj["iso_record_program"] = settings.iso_record_program;
    obj["speaker_sensitivity_ms"] =
        static_cast<double>(settings.speaker_sensitivity_ms);
    obj["speaker_hold_ms"] = static_cast<double>(settings.speaker_hold_ms);
    obj["speaker_require_video"] = settings.speaker_require_video;
    obj["speaker_exclude_participant_1"] =
        static_cast<double>(settings.speaker_exclude_participant_1);
    obj["speaker_exclude_participant_2"] =
        static_cast<double>(settings.speaker_exclude_participant_2);
    return obj;
}

static QJsonObject output_json(const ZoomOutputInfo &output)
{
    QJsonObject obj;
    obj["source_uuid"] = QString::fromStdString(output.source_uuid);
    obj["source_name"] = QString::fromStdString(output.source_name);
    obj["display_name"] = output.display_name.empty()
        ? QString::fromStdString(output.source_name)
        : QString::fromStdString(output.display_name);
    obj["assignment"] = assignment_id(output);
    obj["participant_id"] = static_cast<double>(output.participant_id);
    obj["spotlight_slot"] = static_cast<double>(output.spotlight_slot);
    obj["failover_participant_id"] =
        static_cast<double>(output.failover_participant_id);
    obj["requested_resolution"] = resolution_id(output.video_resolution);
    obj["requested_width"] = static_cast<double>(
        video_resolution_width(output.video_resolution));
    obj["requested_height"] = static_cast<double>(
        video_resolution_height(output.video_resolution));
    obj["negotiated_resolution"] = output.negotiated_resolution;
    obj["negotiated_resolution_label"] = negotiated_text(output);
    obj["observed_width"] = static_cast<double>(output.observed_width);
    obj["observed_height"] = static_cast<double>(output.observed_height);
    obj["observed_fps"] = output.observed_fps;
    obj["last_frame_age_ms"] = static_cast<double>(output.last_frame_age_ms);
    obj["video_stale"] = output.video_stale;
    obj["health_reason"] = output_health_reason_id(output.health_reason);
    obj["health_label"] = output_health_reason_label(output.health_reason);
    obj["duplicate_participant_assignment"] =
        output.duplicate_participant_assignment;
    obj["stale_recovery_attempts"] =
        static_cast<double>(output.stale_recovery_attempts);
    obj["stale_recovery_cooldown_ms"] =
        static_cast<double>(output.stale_recovery_cooldown_ms);
    obj["quality_upgrade_attempts"] =
        static_cast<double>(output.quality_upgrade_attempts);
    obj["quality_upgrade_cooldown_ms"] =
        static_cast<double>(output.quality_upgrade_cooldown_ms);
    obj["subscribed_age_ms"] = static_cast<double>(output.subscribed_age_ms);
    obj["last_set_resolution_code"] = output.last_set_resolution_code;
    obj["last_video_subscribe_code"] = output.last_video_subscribe_code;
    obj["last_raw_status"] = output.last_raw_status;
    obj["last_quality_stage"] =
        QString::fromStdString(output.last_quality_stage);
    obj["last_quality_event_age_ms"] =
        static_cast<double>(output.last_quality_event_age_ms);
    obj["subscription_downgraded"] = output.subscription_downgraded;
    obj["audio_mode"] = output.audio_mode == AudioChannelMode::Stereo
        ? QStringLiteral("stereo")
        : QStringLiteral("mono");
    obj["isolate_audio"] = output.isolate_audio;
    obj["audience_audio"] = output.audience_audio;
    return obj;
}

static QJsonObject output_health_counts_json(
    const std::vector<ZoomOutputInfo> &outputs)
{
    QJsonObject counts;
    for (const auto &output : outputs) {
        const QString key =
            QString::fromUtf8(output_health_reason_id(output.health_reason));
        counts[key] = counts.value(key).toInt() + 1;
    }
    return counts;
}

static bool output_missing_or_stale(const ZoomOutputInfo &output)
{
    return output.health_reason == ZoomOutputHealthReason::WaitingForFirstFrame ||
        output.health_reason == ZoomOutputHealthReason::StaleFrame ||
        output.observed_width == 0 || output.observed_height == 0 ||
        output.video_stale;
}

static bool output_lower_than_requested(const ZoomOutputInfo &output)
{
    return output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution ||
        output.subscription_downgraded ||
        (output.observed_height > 0 &&
         output.observed_height <
             static_cast<uint32_t>(video_resolution_height(output.video_resolution)));
}

static bool output_iso_eligible(const ZoomOutputInfo &output)
{
    if (output.assignment == AssignmentMode::ScreenShare)
        return false;
    if (output.assignment == AssignmentMode::ActiveSpeaker ||
        output.assignment == AssignmentMode::SpotlightIndex)
        return true;
    return output.participant_id != 0;
}

static qint64 estimated_output_bytes_per_second(const ZoomOutputInfo &output)
{
    const int requested_height = video_resolution_height(output.video_resolution);
    const uint32_t observed_height = output.observed_height;
    const int height = static_cast<int>(observed_height > 0
        ? observed_height
        : static_cast<uint32_t>(requested_height));
    if (height >= 1000)
        return 1100ll * 1024ll;
    if (height >= 700)
        return 700ll * 1024ll;
    return 350ll * 1024ll;
}

static QJsonObject output_action_counts_json(
    const std::vector<ZoomOutputInfo> &outputs)
{
    QJsonObject counts;
    int recover_stale = 0;
    int retry_quality = 0;
    int reassign_duplicate = 0;
    int routing_unavailable = 0;
    int video_or_roster_action = 0;
    for (const auto &output : outputs) {
        if (output_missing_or_stale(output))
            ++recover_stale;
        if (output_lower_than_requested(output))
            ++retry_quality;
        if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
            ++reassign_duplicate;
        if (output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable ||
            output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable ||
            output.health_reason == ZoomOutputHealthReason::ScreenShareUnavailable)
            ++routing_unavailable;
        if (output.health_reason == ZoomOutputHealthReason::ParticipantMissing ||
            output.health_reason == ZoomOutputHealthReason::ParticipantVideoOff)
            ++video_or_roster_action;
    }
    counts["recover_stale_or_missing"] = recover_stale;
    counts["retry_quality_upgrade"] = retry_quality;
    counts["reassign_duplicate"] = reassign_duplicate;
    counts["routing_unavailable"] = routing_unavailable;
    counts["participant_video_or_roster"] = video_or_roster_action;
    return counts;
}

static QJsonObject iso_capacity_summary_json(
    const std::vector<ZoomOutputInfo> &outputs,
    const QJsonObject &iso_recorder)
{
    const bool record_program = iso_recorder.value("record_program").toBool();
    int eligible_outputs = 0;
    qint64 estimated_bytes_per_second = 0;
    for (const auto &output : outputs) {
        if (!output_iso_eligible(output))
            continue;
        ++eligible_outputs;
        estimated_bytes_per_second += estimated_output_bytes_per_second(output);
    }
    if (record_program)
        estimated_bytes_per_second += 1100ll * 1024ll;

    const QString output_dir = iso_recorder.value("output_dir").toString();
    const QStorageInfo storage(output_dir);
    const qint64 available = storage.isValid() && storage.isReady()
        ? storage.bytesAvailable()
        : -1;
    const int encode_paths = eligible_outputs + (record_program ? 1 : 0);

    QJsonObject obj;
    obj["eligible_iso_output_count"] = eligible_outputs;
    obj["program_recording_path"] = record_program;
    obj["estimated_h264_path_count"] = encode_paths;
    obj["estimated_bytes_per_second"] =
        static_cast<double>(estimated_bytes_per_second);
    obj["estimated_bytes_per_minute"] =
        static_cast<double>(estimated_bytes_per_second * 60);
    obj["disk_available_bytes"] = static_cast<double>(available);
    obj["estimated_runway_seconds"] =
        available > 0 && estimated_bytes_per_second > 0
            ? static_cast<double>(available / estimated_bytes_per_second)
            : -1.0;
    obj["hardware_encoder_session_pressure"] =
        iso_recorder.value("hardware_encoder").toBool() && encode_paths > 3;
    obj["low_runway_warning"] =
        available > 0 && estimated_bytes_per_second > 0 &&
        available / estimated_bytes_per_second < 30 * 60;
    return obj;
}

static QJsonObject iso_session_health_counts_json(const QJsonArray &sessions)
{
    QJsonObject counts;
    for (const QJsonValue &value : sessions) {
        const QString health = value.toObject()
            .value("session_health").toString(QStringLiteral("unknown"));
        counts[health] = counts.value(health).toInt() + 1;
    }
    return counts;
}

static QJsonObject participant_json(const ParticipantInfo &participant)
{
    QJsonObject obj;
    obj["id"] = static_cast<double>(participant.user_id);
    obj["name"] = QString::fromStdString(participant.display_name);
    obj["has_video"] = participant.has_video;
    obj["is_talking"] = participant.is_talking;
    obj["is_muted"] = participant.is_muted;
    obj["is_host"] = participant.is_host;
    obj["is_co_host"] = participant.is_co_host;
    obj["raised_hand"] = participant.raised_hand;
    obj["spotlight_index"] = static_cast<double>(participant.spotlight_index);
    obj["is_sharing_screen"] = participant.is_sharing_screen;
    return obj;
}

static QJsonObject event_json(const ZoomEngineClient::DebugEvent &event)
{
    QJsonObject obj;
    obj["timestamp_ms"] = static_cast<double>(event.timestamp_ms);
    obj["stage"] = QString::fromStdString(event.stage);
    obj["source_uuid"] = QString::fromStdString(event.source_uuid);
    obj["participant_id"] = static_cast<double>(event.participant_id);
    obj["message"] = QString::fromStdString(event.message);
    return obj;
}

static bool write_text_file(const QString &path, const QString &text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream stream(&file);
    stream << text;
    return true;
}

static bool write_json_file(const QString &path, const QJsonObject &obj)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

static QString obs_module_file_path(const char *name)
{
    char *path = obs_module_file(name);
    const QString result = path ? QString::fromUtf8(path) : QString{};
    if (path)
        bfree(path);
    return result;
}

static QJsonObject runtime_file_json(const QString &label, const QString &path)
{
    const QFileInfo info(path);
    QJsonObject obj;
    obj["label"] = label;
    obj["path"] = path;
    obj["exists"] = info.exists();
    obj["is_file"] = info.isFile();
    obj["size_bytes"] = info.exists() ? static_cast<double>(info.size()) : 0.0;
    obj["last_modified"] = info.exists()
        ? info.lastModified().toString(Qt::ISODate)
        : QString{};
    return obj;
}

static QJsonArray runtime_manifest_json()
{
    QJsonArray arr;
    arr.append(runtime_file_json("OAuth callback helper",
                                 obs_module_file_path("CoreVideoOAuthCallback.exe")));
    arr.append(runtime_file_json("Zoom engine",
                                 obs_module_file_path("zoom-runtime/ZoomObsEngine.exe")));
    arr.append(runtime_file_json("Zoom Meeting SDK runtime",
                                 obs_module_file_path("zoom-runtime/sdk.dll")));
    arr.append(runtime_file_json("Qt Windows platform plugin",
                                 obs_module_file_path("plugins/platforms/qwindows.dll")));
    arr.append(runtime_file_json("Qt Schannel TLS plugin",
                                 obs_module_file_path("plugins/tls/qschannelbackend.dll")));
    return arr;
}

static QString runtime_manifest_text(const QJsonArray &manifest)
{
    QString text;
    QTextStream out(&text);
    out << "Runtime Files\n";
    for (const auto &value : manifest) {
        const QJsonObject obj = value.toObject();
        out << "- " << obj.value("label").toString()
            << " | exists=" << (obj.value("exists").toBool() ? "yes" : "no")
            << " | " << obj.value("path").toString()
            << "\n";
    }
    return text;
}

static QStringList failure_classifications(const ZoomPluginSettings &settings,
                                           const QJsonArray &runtime_manifest,
                                           const std::vector<ZoomOutputInfo> &outputs,
                                           const QJsonObject &iso_recorder,
                                           const QJsonArray &iso_sessions)
{
    QStringList classes;
    const QString last_error =
        QString::fromStdString(ZoomEngineClient::instance().last_error());
    const MeetingState state = ZoomEngineClient::instance().state();

    const bool missing_runtime = std::any_of(
        runtime_manifest.begin(), runtime_manifest.end(),
        [](const QJsonValue &value) {
            return !value.toObject().value("exists").toBool();
        });
    if (missing_runtime)
        classes << QStringLiteral("missing_runtime");

    if (!ZoomEngineClient::instance().is_authenticated() &&
        (settings.oauth_access_token.empty() ||
         settings.resolved_meeting_sdk_public_app_key().empty())) {
        classes << QStringLiteral("auth_not_ready");
    }

    if (state == MeetingState::Failed ||
        last_error.contains(QStringLiteral("auth"), Qt::CaseInsensitive) ||
        last_error.contains(QStringLiteral("join"), Qt::CaseInsensitive) ||
        last_error.contains(QStringLiteral("sdk"), Qt::CaseInsensitive)) {
        classes << QStringLiteral("join_or_sdk_error");
    }

    const bool has_outputs = !outputs.empty();
    const bool all_outputs_missing = has_outputs && std::all_of(
        outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.observed_width == 0 || output.observed_height == 0;
        });
    if (ZoomEngineClient::instance().is_media_active() && all_outputs_missing)
        classes << QStringLiteral("no_video_frames");

    if (std::any_of(outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.video_stale ||
                output.health_reason == ZoomOutputHealthReason::StaleFrame;
        })) {
        classes << QStringLiteral("stale_feed");
    }

    if (std::any_of(outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.subscription_downgraded ||
                output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution;
        })) {
        classes << QStringLiteral("low_quality_feed");
    }

    if (std::any_of(outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.duplicate_participant_assignment ||
                output.health_reason == ZoomOutputHealthReason::DuplicateAssignment;
        })) {
        classes << QStringLiteral("duplicate_assignment");
    }

    if (std::any_of(outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.health_reason == ZoomOutputHealthReason::ActiveSpeakerUnavailable;
        })) {
        classes << QStringLiteral("active_speaker_unavailable");
    }

    if (std::any_of(outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.health_reason == ZoomOutputHealthReason::SpotlightUnavailable;
        })) {
        classes << QStringLiteral("spotlight_unavailable");
    }

    const bool iso_active = iso_recorder.value("active").toBool();
    if (iso_active && iso_sessions.isEmpty())
        classes << QStringLiteral("iso_no_sessions");

    const bool iso_encoder_error = std::any_of(
        iso_sessions.begin(), iso_sessions.end(), [](const QJsonValue &value) {
            return !value.toObject().value("ffmpeg_error").toString().trimmed().isEmpty();
        });
    if (iso_encoder_error)
        classes << QStringLiteral("iso_encoder_error");

    if (std::any_of(iso_sessions.begin(), iso_sessions.end(), [](const QJsonValue &value) {
            return value.toObject().value("session_health").toString() ==
                QStringLiteral("encoder_stopped");
        })) {
        classes << QStringLiteral("iso_encoder_stopped");
    }

    if (std::any_of(iso_sessions.begin(), iso_sessions.end(), [](const QJsonValue &value) {
            return value.toObject().value("session_health").toString() ==
                QStringLiteral("waiting_for_video");
        })) {
        classes << QStringLiteral("iso_waiting_for_video");
    }

    if (std::any_of(iso_sessions.begin(), iso_sessions.end(), [](const QJsonValue &value) {
            return value.toObject().value("session_health").toString() ==
                QStringLiteral("no_audio_yet");
        })) {
        classes << QStringLiteral("iso_no_audio_yet");
    }

    if (iso_recorder.value("disk_warning").toBool())
        classes << QStringLiteral("iso_disk_warning");

    classes.removeDuplicates();
    if (classes.isEmpty())
        classes << QStringLiteral("none_detected");
    return classes;
}

static QString create_support_bundle_zip(const QString &bundle_path,
                                         QStringList &warnings)
{
#if defined(_WIN32)
    const QString zip_path = bundle_path + QStringLiteral(".zip");
    QString program = QStandardPaths::findExecutable("powershell.exe");
    if (program.isEmpty())
        program = QStandardPaths::findExecutable("pwsh.exe");
    if (program.isEmpty()) {
        warnings << QStringLiteral("Could not create zip: PowerShell was not found.");
        return {};
    }

    QProcess zip;
    zip.setProgram(program);
    zip.setArguments({
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command",
        "Compress-Archive -LiteralPath $args[0] -DestinationPath $args[1] -Force",
        bundle_path,
        zip_path,
    });
    zip.setProcessChannelMode(QProcess::MergedChannels);
    zip.start(QIODevice::ReadOnly);
    if (!zip.waitForStarted(5000)) {
        warnings << QString("Could not create zip: %1").arg(zip.errorString());
        return {};
    }
    if (!zip.waitForFinished(30000)) {
        zip.kill();
        warnings << QStringLiteral("Could not create zip: timed out after 30 seconds.");
        return {};
    }
    if (zip.exitStatus() != QProcess::NormalExit || zip.exitCode() != 0) {
        warnings << QString("Could not create zip: %1")
            .arg(QString::fromUtf8(zip.readAll()).trimmed());
        return {};
    }
    return QFileInfo::exists(zip_path) ? zip_path : QString{};
#else
    (void)bundle_path;
    warnings << QStringLiteral("Zip creation is currently only automatic on Windows.");
    return {};
#endif
}

ZoomDiagnosticsDialog::ZoomDiagnosticsDialog(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("Zoom Diagnostics");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(1160, 760);
    resize(1280, 820);

    m_summary = new QLabel(this);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_outputs = new QTableWidget(this);
    m_outputs->setColumnCount(DiagOutputColumnCount);
    m_outputs->setHorizontalHeaderLabels({
        "Output", "Participant", "Assignment", "Requested", "Negotiated",
        "Observed", "FPS", "Frame Age", "Retries", "SDK", "State"
    });
    m_outputs->horizontalHeader()->setSectionResizeMode(DiagOutput, QHeaderView::Stretch);
    for (int col = DiagParticipant; col < DiagOutputColumnCount; ++col)
        m_outputs->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    m_outputs->verticalHeader()->setVisible(false);
    m_outputs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outputs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_outputs->setMinimumHeight(260);

    m_events = new QTableWidget(this);
    m_events->setColumnCount(EventColumnCount);
    m_events->setHorizontalHeaderLabels({
        "Time", "Stage", "Source", "Participant", "Message"
    });
    m_events->horizontalHeader()->setSectionResizeMode(EventMessage, QHeaderView::Stretch);
    for (int col = EventTime; col < EventMessage; ++col)
        m_events->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    m_events->verticalHeader()->setVisible(false);
    m_events->setSelectionMode(QAbstractItemView::SingleSelection);
    m_events->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *refresh_button = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);
    auto *export_button = buttons->addButton("Create Support Bundle",
                                             QDialogButtonBox::ActionRole);
    connect(refresh_button, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(export_button, &QPushButton::clicked, this,
            [this]() { export_diagnostics(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::hide);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(new QLabel("Outputs", this));
    layout->addWidget(m_outputs);
    layout->addWidget(new QLabel("Recent Engine Events", this));
    layout->addWidget(m_events);
    layout->addWidget(buttons);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this]() { refresh(); });
    m_timer->start();

    setStyleSheet(cv_stylesheet());
    refresh();
}

void ZoomDiagnosticsDialog::refresh_now()
{
    refresh();
}

void ZoomDiagnosticsDialog::prepare_shutdown()
{
    if (m_timer)
        m_timer->stop();
}

void ZoomDiagnosticsDialog::refresh()
{
    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto roster = ZoomEngineClient::instance().roster();
    const auto events = ZoomEngineClient::instance().recent_debug_events();

    std::unordered_map<uint32_t, QString> participant_names;
    for (const auto &p : roster) {
        participant_names[p.user_id] = p.display_name.empty()
            ? QString("ID %1").arg(p.user_id)
            : QString::fromStdString(p.display_name);
    }

    m_summary->setText(QString(
        "Meeting: %1   Media: %2   Outputs: %3   Participants: %4   Last error: %5")
        .arg(state_text(ZoomEngineClient::instance().state()))
        .arg(ZoomEngineClient::instance().is_media_active() ? "active" : "inactive")
        .arg(outputs.size())
        .arg(roster.size())
        .arg(QString::fromStdString(ZoomEngineClient::instance().last_error())));

    const TableViewState outputs_state =
        capture_table_view_state(m_outputs, {DiagOutput});
    const QSignalBlocker outputs_signal_blocker(m_outputs);
    m_outputs->setUpdatesEnabled(false);
    m_outputs->setRowCount(static_cast<int>(outputs.size()));
    for (int row = 0; row < static_cast<int>(outputs.size()); ++row) {
        const auto &output = outputs[row];
        const QString source_name = output.display_name.empty()
            ? QString::fromStdString(output.source_name)
            : QString::fromStdString(output.display_name);
        const QString participant = output.participant_id == 0
            ? QStringLiteral("-")
            : participant_names.count(output.participant_id)
                ? participant_names[output.participant_id]
                : QString("ID %1").arg(output.participant_id);

        m_outputs->setItem(row, DiagOutput, readonly_item(source_name));
        m_outputs->setItem(row, DiagParticipant, readonly_item(participant));
        m_outputs->setItem(row, DiagAssignment, readonly_item(assignment_text(output)));
        m_outputs->setItem(row, DiagRequested, readonly_item(resolution_text(output.video_resolution)));
        m_outputs->setItem(row, DiagNegotiated, readonly_item(negotiated_text(output)));
        m_outputs->setItem(row, DiagObserved, readonly_item(observed_text(output)));
        m_outputs->setItem(row, DiagFps, readonly_item(QString::number(output.observed_fps, 'f', 1)));
        m_outputs->setItem(row, DiagAge, readonly_item(output.last_frame_age_ms == 0
            ? QStringLiteral("-") : QString("%1 ms").arg(output.last_frame_age_ms)));
        m_outputs->setItem(row, DiagRetries, readonly_item(retry_text(output)));
        auto *sdk_item = readonly_item(sdk_text(output));
        if (output.last_video_subscribe_code > 0)
            sdk_item->setForeground(QColor(255, 107, 107));
        else if (output.subscription_downgraded)
            sdk_item->setForeground(QColor(240, 180, 41));
        m_outputs->setItem(row, DiagSdk, sdk_item);
        auto *state_item = readonly_item(signal_state_text(output));
        if (output.health_reason == ZoomOutputHealthReason::DuplicateAssignment)
            state_item->setForeground(QColor(255, 107, 107));
        else if (output.health_reason == ZoomOutputHealthReason::StaleFrame)
            state_item->setForeground(Qt::red);
        else if (output.health_reason == ZoomOutputHealthReason::ZoomDeliveredLowerResolution)
            state_item->setForeground(QColor(240, 180, 41));
        m_outputs->setItem(row, DiagState, state_item);
    }
    restore_table_view_state(m_outputs, outputs_state, {DiagOutput});
    m_outputs->setUpdatesEnabled(true);

    const int max_rows = std::min<int>(static_cast<int>(events.size()), 150);
    const TableViewState events_state = capture_table_view_state(
        m_events, {EventTime, EventStage, EventSource, EventMessage});
    const QSignalBlocker events_signal_blocker(m_events);
    m_events->setUpdatesEnabled(false);
    m_events->setRowCount(max_rows);
    for (int row = 0; row < max_rows; ++row) {
        const auto &event = events[events.size() - 1 - row];
        m_events->setItem(row, EventTime,
                          readonly_item(QString::number(event.timestamp_ms)));
        m_events->setItem(row, EventStage,
                          readonly_item(QString::fromStdString(event.stage)));
        m_events->setItem(row, EventSource,
                          readonly_item(QString::fromStdString(event.source_uuid)));
        m_events->setItem(row, EventParticipant,
                          readonly_item(event.participant_id == 0
                              ? QStringLiteral("-")
                              : QString::number(event.participant_id)));
        auto *message = readonly_item(QString::fromStdString(event.message));
        message->setToolTip(QString::fromStdString(event.message));
        m_events->setItem(row, EventMessage, message);
    }
    restore_table_view_state(m_events, events_state,
                             {EventTime, EventStage, EventSource, EventMessage});
    m_events->setUpdatesEnabled(true);
}

void ZoomDiagnosticsDialog::export_diagnostics()
{
    const auto outputs = ZoomOutputManager::instance().outputs();
    const auto roster = ZoomEngineClient::instance().roster();
    const auto events = ZoomEngineClient::instance().recent_debug_events();
    const auto settings = ZoomPluginSettings::load();
    const QJsonObject iso_recorder =
        ZoomIsoRecorder::instance().status_overview();
    const QJsonArray iso_sessions =
        ZoomIsoRecorder::instance().status_json();
    const QJsonObject output_action_counts =
        output_action_counts_json(outputs);
    const QJsonObject iso_capacity =
        iso_capacity_summary_json(outputs, iso_recorder);
    const QJsonObject iso_session_health_counts =
        iso_session_health_counts_json(iso_sessions);
    const QDateTime now = QDateTime::currentDateTime();
    const QString stamp = now.toString("yyyyMMdd-HHmmss");
    QStringList warnings;
    QDir root(diagnostics_root_dir());
    if (!root.exists() && !root.mkpath(".")) {
        QMessageBox::warning(this, "Create Support Bundle",
            QString("Could not create support bundle folder:\n%1")
                .arg(root.absolutePath()));
        return;
    }

    const QString bundle_path = root.absoluteFilePath(
        QStringLiteral("CoreVideo-support-%1").arg(stamp));
    QDir bundle(bundle_path);
    if (!bundle.exists() &&
        !root.mkpath(QStringLiteral("CoreVideo-support-%1").arg(stamp))) {
        QMessageBox::warning(this, "Create Support Bundle",
            QString("Could not create support bundle:\n%1")
                .arg(bundle_path));
        return;
    }

    QJsonArray output_array;
    for (const auto &output : outputs)
        output_array.append(output_json(output));

    QJsonArray roster_array;
    for (const auto &participant : roster)
        roster_array.append(participant_json(participant));

    QJsonArray event_array;
    for (const auto &event : events)
        event_array.append(event_json(event));
    const QJsonArray runtime_manifest = runtime_manifest_json();
    for (const auto &value : runtime_manifest) {
        const QJsonObject obj = value.toObject();
        if (!obj.value("exists").toBool()) {
            warnings << QString("Runtime file missing: %1 (%2)")
                .arg(obj.value("label").toString(),
                     obj.value("path").toString());
        }
    }

    const QString obs_log = latest_obs_log_path();
    const QString copied_log_name = obs_log.isEmpty()
        ? QString{}
        : QStringLiteral("obs-latest.log");
    if (!obs_log.isEmpty()) {
        const QString excerpt = redacted_obs_log_excerpt(obs_log, warnings);
        if (!excerpt.isEmpty() &&
            !write_text_file(bundle.absoluteFilePath(copied_log_name), excerpt)) {
            warnings << QStringLiteral("Could not write redacted OBS log excerpt from %1")
                .arg(obs_log);
        }
    }

    const QStringList classifications =
        failure_classifications(settings, runtime_manifest, outputs,
                                iso_recorder, iso_sessions);
    QJsonArray classification_array;
    for (const QString &classification : classifications)
        classification_array.append(classification);

    QJsonObject engine_status;
    engine_status["running"] = ZoomEngineClient::instance().is_running();
    engine_status["meeting_state"] = state_text(ZoomEngineClient::instance().state());
    engine_status["media_active"] = ZoomEngineClient::instance().is_media_active();
    engine_status["authenticated"] = ZoomEngineClient::instance().is_authenticated();
    engine_status["active_speaker_id"] =
        static_cast<double>(ZoomEngineClient::instance().active_speaker_id());
    engine_status["raw_active_speaker_id"] =
        static_cast<double>(ZoomEngineClient::instance().raw_active_speaker_id());
    engine_status["last_error"] =
        QString::fromStdString(ZoomEngineClient::instance().last_error());
    engine_status["output_count"] = static_cast<double>(outputs.size());
    engine_status["participant_count"] = static_cast<double>(roster.size());
    engine_status["recent_engine_event_count"] =
        static_cast<double>(events.size());
    engine_status["iso_recorder"] = iso_recorder;
    engine_status["iso_session_count"] =
        iso_recorder.value("session_count").toInt();
    engine_status["iso_completed_session_count"] =
        iso_recorder.value("completed_session_count").toInt();
    engine_status["iso_capacity"] = iso_capacity;
    engine_status["iso_session_health_counts"] = iso_session_health_counts;
    const int unhealthy_outputs = static_cast<int>(std::count_if(
        outputs.begin(), outputs.end(), [](const ZoomOutputInfo &output) {
            return output.health_reason != ZoomOutputHealthReason::Ok;
        }));
    engine_status["unhealthy_output_count"] = unhealthy_outputs;
    engine_status["output_health_counts"] = output_health_counts_json(outputs);
    engine_status["output_action_counts"] = output_action_counts;

    QJsonObject summary;
    summary["bundle_type"] = QStringLiteral("corevideo_support_bundle");
    summary["created_at"] = now.toString(Qt::ISODate);
    summary["plugin_version"] = OBS_ZOOM_PLUGIN_VERSION;
    summary["root"] = root.absolutePath();
    summary["bundle_path"] = bundle.absolutePath();
    summary["meeting_state"] = state_text(ZoomEngineClient::instance().state());
    summary["media_active"] = ZoomEngineClient::instance().is_media_active();
    summary["engine_running"] = ZoomEngineClient::instance().is_running();
    summary["authenticated"] = ZoomEngineClient::instance().is_authenticated();
    summary["active_speaker_id"] =
        static_cast<double>(ZoomEngineClient::instance().active_speaker_id());
    summary["raw_active_speaker_id"] =
        static_cast<double>(ZoomEngineClient::instance().raw_active_speaker_id());
    summary["last_error"] =
        QString::fromStdString(ZoomEngineClient::instance().last_error());
    summary["obs_log_source"] = obs_log;
    summary["obs_log_copy"] = copied_log_name;
    QJsonArray warning_array;
    for (const QString &warning : warnings)
        warning_array.append(warning);
    summary["warnings"] = warning_array;
    summary["failure_classifications"] = classification_array;
    summary["runtime_manifest"] = runtime_manifest;
    summary["engine_status"] = engine_status;
    summary["output_health_counts"] = output_health_counts_json(outputs);
    summary["output_action_counts"] = output_action_counts;
    summary["plugin_settings_redacted"] = settings_json(settings);
    summary["outputs"] = output_array;
    summary["participants"] = roster_array;
    summary["recent_engine_events"] = event_array;
    summary["iso_recorder"] = iso_recorder;
    summary["iso_capacity"] = iso_capacity;
    summary["iso_session_health_counts"] = iso_session_health_counts;
    summary["iso_sessions"] = iso_sessions;

    if (!write_json_file(bundle.absoluteFilePath("summary.json"), summary) ||
        !write_json_file(bundle.absoluteFilePath("engine-status.json"),
                         engine_status) ||
        !write_json_file(bundle.absoluteFilePath("settings-redacted.json"),
                         settings_json(settings)) ||
        !write_json_file(bundle.absoluteFilePath("runtime-manifest.json"),
                         QJsonObject{{"files", runtime_manifest}}) ||
        !write_json_file(bundle.absoluteFilePath("iso-recorder.json"),
                         QJsonObject{{"recorder", iso_recorder},
                                     {"capacity", iso_capacity},
                                     {"session_health_counts", iso_session_health_counts},
                                     {"sessions", iso_sessions}})) {
        QMessageBox::warning(this, "Create Support Bundle",
            QString("Could not write support bundle JSON files in:\n%1")
                .arg(bundle.absolutePath()));
        return;
    }

    QString text;
    QTextStream out(&text);
    out << "CoreVideo Support Bundle\n";
    out << "Created: " << now.toString(Qt::ISODate) << "\n";
    out << "Bundle: " << bundle.absolutePath() << "\n";
    out << "Plugin version: " << OBS_ZOOM_PLUGIN_VERSION << "\n";
    out << "Engine running: "
        << (ZoomEngineClient::instance().is_running() ? "yes" : "no")
        << "\n";
    out << "Meeting: " << state_text(ZoomEngineClient::instance().state()) << "\n";
    out << "Media: "
        << (ZoomEngineClient::instance().is_media_active() ? "active" : "inactive")
        << "\n";
    out << "Authenticated: "
        << (ZoomEngineClient::instance().is_authenticated() ? "yes" : "no")
        << "\n";
    out << "Active speaker: " << ZoomEngineClient::instance().active_speaker_id()
        << "\n";
    out << "Raw active speaker: "
        << ZoomEngineClient::instance().raw_active_speaker_id() << "\n";
    out << "Last error: "
        << QString::fromStdString(ZoomEngineClient::instance().last_error())
        << "\n";
    out << "OBS log: " << (obs_log.isEmpty() ? QStringLiteral("not found") : obs_log)
        << "\n\n";
    out << "Failure classifications\n";
    for (const QString &classification : classifications)
        out << "- " << classification << "\n";
    out << "\n";
    if (!warnings.isEmpty()) {
        out << "Warnings\n";
        for (const QString &warning : warnings)
            out << "- " << warning << "\n";
        out << "\n";
    }

    out << "Settings\n";
    out << "- Meeting SDK auth mode: "
        << QString::fromStdString(settings.meeting_sdk_auth_mode) << "\n";
    out << "- Public app key: "
        << redacted_value(settings.resolved_meeting_sdk_public_app_key())
        << "\n";
    out << "- OAuth access token: "
        << (settings.oauth_access_token.empty() ? "not set" : "set/redacted")
        << "\n";
    out << "- Control server port: " << settings.control_server_port << "\n";
    out << "- OSC server port: " << settings.osc_server_port << "\n";
    out << "- Hardware acceleration: " << hw_accel_mode_id(settings.hw_accel_mode)
        << "\n";
    out << "- Reconnect: "
        << (settings.reconnect_policy.enabled ? "enabled" : "disabled")
        << "\n\n";

    out << runtime_manifest_text(runtime_manifest) << "\n";

    out << "Output Actions\n";
    out << "- Recover stale/missing: "
        << output_action_counts.value("recover_stale_or_missing").toInt()
        << "\n";
    out << "- Retry quality upgrade: "
        << output_action_counts.value("retry_quality_upgrade").toInt()
        << "\n";
    out << "- Reassign duplicate: "
        << output_action_counts.value("reassign_duplicate").toInt()
        << "\n";
    out << "- Routing unavailable: "
        << output_action_counts.value("routing_unavailable").toInt()
        << "\n";
    out << "- Participant video/roster action: "
        << output_action_counts.value("participant_video_or_roster").toInt()
        << "\n\n";

    out << "ISO Recorder\n";
    out << "- Active: "
        << (iso_recorder.value("active").toBool() ? "yes" : "no") << "\n";
    out << "- Requested encoder: "
        << iso_recorder.value("requested_video_encoder").toString() << "\n";
    out << "- Active encoder: "
        << iso_recorder.value("video_encoder").toString()
        << (iso_recorder.value("encoder_fallback").toBool()
                ? QStringLiteral(" (fallback)")
                : QString{})
        << "\n";
    out << "- Hardware encoder: "
        << (iso_recorder.value("hardware_encoder").toBool() ? "yes" : "no")
        << "\n";
    out << "- Program recording: "
        << (iso_recorder.value("record_program").toBool() ? "enabled" : "off")
        << "\n";
    out << "- Estimated H.264 paths: "
        << iso_capacity.value("estimated_h264_path_count").toInt()
        << " (ISO outputs "
        << iso_capacity.value("eligible_iso_output_count").toInt()
        << ")\n";
    out << "- Estimated disk rate: "
        << QString::number(
               iso_capacity.value("estimated_bytes_per_minute").toDouble() /
               (1024.0 * 1024.0), 'f', 1)
        << " MB/min\n";
    const double runway_seconds =
        iso_capacity.value("estimated_runway_seconds").toDouble(-1.0);
    if (runway_seconds >= 0.0) {
        out << "- Estimated disk runway: "
            << QString::number(runway_seconds / 60.0, 'f', 1)
            << " min"
            << (iso_capacity.value("low_runway_warning").toBool()
                    ? QStringLiteral(" (low)")
                    : QString{})
            << "\n";
    }
    if (iso_capacity.value("hardware_encoder_session_pressure").toBool())
        out << "- Capacity warning: hardware encoder session pressure likely\n";
    out << "- Session health:";
    if (iso_session_health_counts.isEmpty()) {
        out << " none\n";
    } else {
        for (auto it = iso_session_health_counts.begin();
             it != iso_session_health_counts.end(); ++it) {
            out << " " << it.key() << "=" << it.value().toInt();
        }
        out << "\n";
    }
    out << "- Sessions: active "
        << iso_recorder.value("session_count").toInt()
        << ", completed "
        << iso_recorder.value("completed_session_count").toInt()
        << "\n";
    const QString iso_warning = iso_recorder.value("warning").toString();
    if (!iso_warning.isEmpty())
        out << "- Warning: " << iso_warning << "\n";
    for (const QJsonValue &value : iso_sessions) {
        const QJsonObject session = value.toObject();
        out << "- "
            << (session.value("completed").toBool() ? "completed" : "recording")
            << " | " << session.value("display_name").toString()
            << " | source " << session.value("source").toString()
            << " | participant "
            << QString::number(
                   static_cast<qulonglong>(
                       session.value("resolved_participant_id").toDouble()))
            << " | " << session.value("width").toInt()
            << "x" << session.value("height").toInt()
            << " | frames " << session.value("video_frames").toInt()
            << " | audio chunks " << session.value("audio_chunks").toInt()
            << " | encoder " << session.value("video_encoder").toString()
            << " | health " << session.value("session_health").toString()
            << " | video " << session.value("video_path").toString()
            << " | audio " << session.value("audio_path").toString()
            << "\n";
        const QString ffmpeg_error =
            session.value("ffmpeg_error").toString().trimmed();
        if (!ffmpeg_error.isEmpty())
            out << "  ffmpeg error: " << ffmpeg_error << "\n";
    }
    out << "\n";

    out << "Outputs\n";
    for (const auto &output : outputs) {
        out << "- " << (output.display_name.empty()
                ? QString::fromStdString(output.source_name)
                : QString::fromStdString(output.display_name))
            << " | " << assignment_text(output)
            << " | participant " << output.participant_id
            << " | requested " << resolution_text(output.video_resolution)
            << " | negotiated " << negotiated_text(output)
            << " | observed " << observed_text(output)
            << " | fps " << QString::number(output.observed_fps, 'f', 1)
            << " | sdk " << sdk_text(output)
            << " | " << output_health_reason_label(output.health_reason)
            << "\n";
    }

    out << "\nParticipants\n";
    for (const auto &participant : roster) {
        out << "- " << QString::fromStdString(participant.display_name)
            << " (" << participant.user_id << ")"
            << " video=" << (participant.has_video ? "on" : "off")
            << " audio=" << (participant.is_muted ? "muted" : "open")
            << " talking=" << (participant.is_talking ? "yes" : "no")
            << " sharing=" << (participant.is_sharing_screen ? "yes" : "no")
            << "\n";
    }

    out << "\nRecent Engine Events\n";
    const int start = std::max<int>(0, static_cast<int>(events.size()) - 50);
    for (int i = start; i < static_cast<int>(events.size()); ++i) {
        const auto &event = events[i];
        out << "- " << event.timestamp_ms
            << " " << QString::fromStdString(event.stage)
            << " source=" << QString::fromStdString(event.source_uuid)
            << " participant=" << event.participant_id
            << " " << QString::fromStdString(event.message)
            << "\n";
    }

    if (!write_text_file(bundle.absoluteFilePath("summary.txt"), text)) {
        QMessageBox::warning(this, "Create Support Bundle",
            QString("Could not write summary.txt in:\n%1")
                .arg(bundle.absolutePath()));
        return;
    }

    const QString zip_path = create_support_bundle_zip(bundle.absolutePath(),
                                                       warnings);
    if (!zip_path.isEmpty()) {
        summary["zip_path"] = zip_path;
        warning_array = QJsonArray();
        for (const QString &warning : warnings)
            warning_array.append(warning);
        summary["warnings"] = warning_array;
        write_json_file(bundle.absoluteFilePath("summary.json"), summary);
    }

    QMessageBox::information(this, "Create Support Bundle",
        zip_path.isEmpty()
            ? QString("Support bundle created at:\n%1").arg(bundle.absolutePath())
            : QString("Support bundle created at:\n%1\n\nFolder:\n%2")
                  .arg(zip_path, bundle.absolutePath()));
}
