#pragma once
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

// A single OBS request step inside a macro
struct MacroStep {
    QString requestType;   // e.g. "SetCurrentProgramScene"
    QJsonObject data;      // requestData fields

    static MacroStep fromJson(const QJsonObject &obj) {
        MacroStep s;
        s.requestType = obj["requestType"].toString();
        s.data = obj["data"].toObject();
        return s;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["requestType"] = requestType;
        obj["data"] = data;
        return obj;
    }
};

// A named macro — fires one or more OBS requests in sequence
struct Macro {
    QString id;
    QString label;
    QString icon;       // single emoji/glyph shown on button
    QString color;      // hex color for button accent, e.g. "#2979ff"
    QVector<MacroStep> steps;

    bool isValid() const { return !id.isEmpty() && !steps.isEmpty(); }

    static Macro fromJson(const QJsonObject &obj) {
        Macro m;
        m.id    = obj["id"].toString();
        m.label = obj["label"].toString();
        m.icon  = obj["icon"].toString();
        m.color = obj["color"].toString();
        const QJsonArray arr = obj["steps"].toArray();
        for (const auto &v : arr)
            m.steps.append(MacroStep::fromJson(v.toObject()));
        return m;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"]    = id;
        obj["label"] = label;
        obj["icon"]  = icon;
        obj["color"] = color;
        QJsonArray arr;
        for (const auto &s : steps)
            arr.append(s.toJson());
        obj["steps"] = arr;
        return obj;
    }

    // Built-in default macros
    static QVector<Macro> defaults() {
        QVector<Macro> list;

        // 1. Go Live
        {
            Macro m;
            m.id    = "go-live";
            m.label = "Go Live";
            m.icon  = "⏩";  // ⏩
            m.color = "#e03040";
            MacroStep s;
            s.requestType = "StartStream";
            m.steps.append(s);
            list.append(m);
        }

        // 2. End Stream
        {
            Macro m;
            m.id    = "end-stream";
            m.label = "End Stream";
            m.icon  = "⏹";  // ⏹
            m.color = "#803020";
            MacroStep s;
            s.requestType = "StopStream";
            m.steps.append(s);
            list.append(m);
        }

        // 3. Record
        {
            Macro m;
            m.id    = "start-record";
            m.label = "Record";
            m.icon  = "⏺";  // ⏺
            m.color = "#e05020";
            MacroStep s;
            s.requestType = "StartRecord";
            m.steps.append(s);
            list.append(m);
        }

        // 4. Stop (recording)
        {
            Macro m;
            m.id    = "stop-record";
            m.label = "Stop";
            m.icon  = "⏹";  // ⏹
            m.color = "#604030";
            MacroStep s;
            s.requestType = "StopRecord";
            m.steps.append(s);
            list.append(m);
        }

        // 5. V-Cam
        {
            Macro m;
            m.id    = "toggle-vcam";
            m.label = "V-Cam";
            m.icon  = "📷";  // 📷
            m.color = "#2060c0";
            MacroStep s;
            s.requestType = "ToggleVirtualCam";
            m.steps.append(s);
            list.append(m);
        }

        // 6. Lower Thirds
        {
            Macro m;
            m.id    = "scene-lower-thirds";
            m.label = "Lower Thirds";
            m.icon  = "▼";  // ▼
            m.color = "#6030c0";
            MacroStep s;
            s.requestType = "SetCurrentProgramScene";
            s.data["sceneName"] = "Lower Thirds";
            m.steps.append(s);
            list.append(m);
        }

        return list;
    }
};
