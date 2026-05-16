#include "lower-third-controller.h"
#include <QCoreApplication>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Look look;
    look.id = "test";
    look.name = "Test";
    look.tileStyle.showNameTag = true;
    look.tmpl.id = "2-up";
    look.tmpl.name = "2-up";
    look.tmpl.slotList.append({0, 0.0, 0.0, 0.5, 1.0, "Slot 1", false});
    look.tmpl.slotList.append({1, 0.5, 0.0, 0.5, 1.0, "Slot 2", false});
    look.slotAssignments.append({0, 101});

    ParticipantInfo p;
    p.id = 101;
    p.name = "Alex Rivera";
    p.color = QColor("#2979ff");
    p.hasVideo = true;

    LowerThirdController ctl;
    const QVector<Overlay> overlays = ctl.participantSyncedOverlays(look, {p});
    if (overlays.size() != 1) {
        std::cerr << "Expected one lower third, got " << overlays.size() << "\n";
        return 1;
    }
    if (!LowerThirdController::isAutoLowerThird(overlays[0])) {
        std::cerr << "Expected generated lower third id\n";
        return 1;
    }
    if (overlays[0].text1 != "Alex Rivera") {
        std::cerr << "Expected participant name in lower third\n";
        return 1;
    }
    return 0;
}
