#pragma once
#include <QString>
#include <QColor>
#include <QVector>
#include <QJsonObject>

// Broadcast overlay — a graphic that sits above the slot composition:
// lower-third, bug, ticker, title card, bumper. All positions in
// 0..1 canvas-relative coordinates so a Look composes the same on any
// output resolution.
//
// For slice 3, overlays are rendered sidecar-side only (preview). A
// later slice will mirror them to OBS as text / image sources.
struct Overlay {
    enum Type { LowerThird, Bug, Ticker, TitleCard, Bumper };

    QString id;            // stable id (preset id, or generated)
    Type    type = LowerThird;
    QString text1;
    QString text2;
    QColor  accent;        // empty/invalid → inherit theme accent
    double  x = 0.0;       // left, 0..1
    double  y = 0.0;       // top, 0..1
    double  w = 1.0;
    double  h = 0.12;
    int     durationMs = 0;  // 0 = sticky, >0 = auto-out (later slice)

    static QString typeToString(Type t)
    {
        switch (t) {
        case LowerThird: return "LowerThird";
        case Bug:        return "Bug";
        case Ticker:     return "Ticker";
        case TitleCard:  return "TitleCard";
        case Bumper:     return "Bumper";
        }
        return "LowerThird";
    }
    static Type typeFromString(const QString &s)
    {
        if (s == "Bug")       return Bug;
        if (s == "Ticker")    return Ticker;
        if (s == "TitleCard") return TitleCard;
        if (s == "Bumper")    return Bumper;
        return LowerThird;
    }

    static Overlay fromJson(const QJsonObject &o)
    {
        Overlay v;
        v.id          = o.value("id").toString();
        v.type        = typeFromString(o.value("type").toString("LowerThird"));
        v.text1       = o.value("text1").toString();
        v.text2       = o.value("text2").toString();
        const QString acc = o.value("accent").toString();
        if (!acc.isEmpty()) v.accent = QColor::fromString(acc);
        v.x           = o.value("x").toDouble(0.0);
        v.y           = o.value("y").toDouble(0.0);
        v.w           = o.value("w").toDouble(1.0);
        v.h           = o.value("h").toDouble(0.12);
        v.durationMs  = o.value("durationMs").toInt(0);
        return v;
    }

    QJsonObject toJson() const
    {
        QJsonObject o{
            {"id", id},
            {"type", typeToString(type)},
            {"text1", text1},
            {"text2", text2},
            {"x", x},
            {"y", y},
            {"w", w},
            {"h", h},
            {"durationMs", durationMs},
        };
        if (accent.isValid())
            o["accent"] = accent.name();
        return o;
    }

    // Curated presets shown in OverlayPanel.
    static QVector<Overlay> builtInPresets()
    {
        QVector<Overlay> v;

        Overlay lt;
        lt.id = "lt-name-title"; lt.type = LowerThird;
        lt.text1 = "Guest Name"; lt.text2 = "Title • Organization";
        lt.x = 0.04; lt.y = 0.78; lt.w = 0.50; lt.h = 0.12;
        v.append(lt);

        Overlay live;
        live.id = "bug-live"; live.type = Bug;
        live.text1 = "● LIVE";
        live.accent = QColor("#e03040");
        live.x = 0.91; live.y = 0.04; live.w = 0.07; live.h = 0.05;
        v.append(live);

        Overlay onair;
        onair.id = "bug-onair"; onair.type = Bug;
        onair.text1 = "ON AIR";
        onair.accent = QColor("#e0a020");
        onair.x = 0.04; onair.y = 0.04; onair.w = 0.08; onair.h = 0.05;
        v.append(onair);

        Overlay tc;
        tc.id = "tc-breaking"; tc.type = TitleCard;
        tc.text1 = "BREAKING"; tc.text2 = "Developing Story";
        tc.accent = QColor("#e03040");
        tc.x = 0.0; tc.y = 0.82; tc.w = 1.0; tc.h = 0.14;
        v.append(tc);

        Overlay ti;
        ti.id = "ticker-headlines"; ti.type = Ticker;
        ti.text1 = "● LIVE — UPDATES CONTINUE — STAY WITH US FOR THE LATEST";
        ti.x = 0.0; ti.y = 0.94; ti.w = 1.0; ti.h = 0.05;
        v.append(ti);

        Overlay coming;
        coming.id = "bumper-coming-up"; coming.type = Bumper;
        coming.text1 = "COMING UP NEXT";
        coming.text2 = "After the break";
        coming.x = 0.0; coming.y = 0.36; coming.w = 1.0; coming.h = 0.28;
        v.append(coming);

        return v;
    }

    static QString humanLabel(const Overlay &o)
    {
        const QString t = typeToString(o.type);
        if (!o.text1.isEmpty()) return QString("%1: %2").arg(t, o.text1);
        return t;
    }
};
