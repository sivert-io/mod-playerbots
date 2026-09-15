/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REALISTICTURNING_H
#define PLAYERBOTS_REALISTICTURNING_H

#include "Define.h"

class Player;
class PlayerbotAI;

// Human-like direction changes for bot point movement (AiPlayerbot.RealisticTurning.*).
//
// Why this exists: a point move launches an SMSG_MONSTER_MOVE spline, and the 3.3.5 client
// orients the model along the first spline segment the moment the packet arrives. Nothing
// in the core rotates a unit gradually, so any heading change (up to a full 180) is shown
// as an instant snap followed by running. Players instead turn at ~pi rad/s, either on the
// spot (A/D) or while running (W + mouse), which traces an arc.
namespace RealisticTurning
{
    // Tries to take over a ground point move for the bot. Returns false when the upstream
    // MovePoint should be used instead (feature off, not applicable, or smoothing would not
    // change anything). extraDelayMs receives the time spent turning in place before the
    // bot actually starts moving, so "last movement" waits long enough.
    bool TryMove(PlayerbotAI* botAI, Player* bot, float x, float y, float z, uint32& extraDelayMs);

    // Invalidates a pending in-place turn. Must be called before any other movement is given
    // to the bot, otherwise the delayed turn event could later launch an outdated path.
    void CancelTurn(PlayerbotAI* botAI, Player* bot);
}

#endif
