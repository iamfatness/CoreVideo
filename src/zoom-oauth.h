#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <string>

class ZoomOAuthManager : public QObject {
    Q_OBJECT
public:
    static ZoomOAuthManager &instance();

    bool begin_authorization(QString *error = nullptr);
    bool handle_redirect_url(const QString &url, QString *error = nullptr);
    bool register_url_scheme(QString *error = nullptr);
    bool refresh_access_token_blocking(QString *error = nullptr);
    bool fetch_zak_blocking(std::string &zak,
                            const std::string &meeting_id = {},
                            QString *error = nullptr);
    bool fetch_sdk_jwt_blocking(std::string &jwt, QString *error = nullptr);

signals:
    void tokens_saved();

private:
    explicit ZoomOAuthManager(QObject *parent = nullptr);

    static QString random_base64url(int byte_count);
    static QString pkce_challenge(const QString &verifier);
    static QString form_encode(const QMap<QString, QString> &fields);
    bool parse_token_response(const QByteArray &body, QString *error);

    QString m_pending_state;
    QString m_pending_verifier;
    QString m_pending_client_id;
    QString m_pending_broker_base_url;
};
