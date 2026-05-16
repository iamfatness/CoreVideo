#pragma once
#include <QString>

// Per-source audio routing — matches the plugin's tri-state model.
// Mixed   : full meeting mix (default).
// Isolated: only the bound participant's voice.
// Audience: residual active speaker — every participant NOT bound to any
//           isolated source. Use one Audience source as a "fallback mic"
//           for overflow speakers when iso channels are spoken for.
enum class AudioRouting { Mixed, Isolated, Audience };

inline QString audioRoutingLabel(AudioRouting r)
{
    switch (r) {
    case AudioRouting::Isolated: return QStringLiteral("Iso");
    case AudioRouting::Audience: return QStringLiteral("Aud");
    case AudioRouting::Mixed:    break;
    }
    return QStringLiteral("Mix");
}

inline QChar audioRoutingBadge(AudioRouting r)
{
    switch (r) {
    case AudioRouting::Isolated: return QChar('I');
    case AudioRouting::Audience: return QChar('A');
    case AudioRouting::Mixed:    break;
    }
    return QChar('M');
}

inline AudioRouting nextAudioRouting(AudioRouting r)
{
    switch (r) {
    case AudioRouting::Mixed:    return AudioRouting::Isolated;
    case AudioRouting::Isolated: return AudioRouting::Audience;
    case AudioRouting::Audience: break;
    }
    return AudioRouting::Mixed;
}
