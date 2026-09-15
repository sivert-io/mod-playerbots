/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RealisticTurning.h"

#include "AiObjectContext.h"
#include "Duration.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInitArgs.h"
#include "MovementGenerator.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "G3D/Vector3.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr float PI_F = static_cast<float>(M_PI);
constexpr float MIN_IN_PLACE_TURN = PI_F / 36.0f;  // 5 deg; below this a turn is not noticeable
// Corners far down the path are usually outside view by the time the bot gets there and the
// bot will likely re-path before reaching them, so smoothing them only costs LOS checks.
constexpr int MAX_SMOOTHED_CORNERS = 8;

float SignedAngle(float a)
{
    a = Position::NormalizeOrientation(a);
    if (a > PI_F)
        a -= 2.0f * PI_F;
    return a;
}

float Bearing(G3D::Vector3 const& from, G3D::Vector3 const& to)
{
    return Position::NormalizeOrientation(std::atan2(to.y - from.y, to.x - from.x));
}

float Dist2d(G3D::Vector3 const& a, G3D::Vector3 const& b) { return std::hypot(b.x - a.x, b.y - a.y); }

LastMovement& GetLastMovement(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
}

bool HasClearSegment(Player* bot, G3D::Vector3 const& a, G3D::Vector3 const& b)
{
    // Ray at half body height: a ground-level ray fails on every small bump, a head-height
    // ray misses low walls and fences the spline would walk through.
    float const h = bot->GetCollisionHeight() * 0.5f;
    return bot->GetMap()->isInLineOfSight(a.x, a.y, a.z + h, b.x, b.y, b.z + h, bot->GetPhaseMask(),
                                          LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}

// Generated arc points are off the navmesh path, so they need their own sanity check.
// A full PathGenerator query per point would be far more expensive than the arc is worth;
// height + slope + water + LOS catches the realistic failure cases (walls, ledges, lakes).
// Returns the ground z, or INVALID_HEIGHT if the step should not be walked.
float ValidateStep(Player* bot, G3D::Vector3 const& prev, float x, float y)
{
    float const z = bot->GetMapHeight(x, y, prev.z + 2.0f);
    if (z <= INVALID_HEIGHT)
        return INVALID_HEIGHT;

    float const step = std::hypot(x - prev.x, y - prev.y);
    if (std::fabs(z - prev.z) > std::max(1.0f, step))  // steeper than ~45 deg, or a ledge
        return INVALID_HEIGHT;

    if (bot->GetMap()->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
        return INVALID_HEIGHT;

    if (!HasClearSegment(bot, prev, G3D::Vector3(x, y, z)))
        return INVALID_HEIGHT;

    return z;
}

// Constant-radius turn (what a player holding W while turning traces) starting at `start`
// facing `heading`, turning towards `dir` (+1 left / counter-clockwise, -1 right), until the
// bearing to `target` is within half a step of the current heading. Appends points to `out`.
bool BuildLeadIn(Player* bot, G3D::Vector3 const& start, float heading, float dir, float radius, float stepAngle,
                 float maxTurn, G3D::Vector3 const& target, Movement::PointsArray& out)
{
    float const cx = start.x + radius * std::cos(heading + dir * PI_F * 0.5f);
    float const cy = start.y + radius * std::sin(heading + dir * PI_F * 0.5f);

    // A target inside the turning circle can never be faced by running along it; the bot
    // would orbit. That case needs an in-place turn instead.
    if (std::hypot(target.x - cx, target.y - cy) <= radius * 1.1f)
        return false;

    G3D::Vector3 last = start;
    float h = heading;
    int const maxSteps = static_cast<int>(std::ceil((maxTurn + PI_F * 0.5f) / stepAngle));
    for (int k = 1; k <= maxSteps; ++k)
    {
        if (std::fabs(SignedAngle(Bearing(last, target) - h)) <= stepAngle * 0.5f)
            return true;

        h = heading + dir * stepAngle * static_cast<float>(k);
        float const px = cx + radius * std::cos(h - dir * PI_F * 0.5f);
        float const py = cy + radius * std::sin(h - dir * PI_F * 0.5f);
        float const pz = ValidateStep(bot, last, px, py);
        if (pz <= INVALID_HEIGHT)
            return false;

        last = G3D::Vector3(px, py, pz);
        out.push_back(last);
    }

    return false;
}

// Replaces sharp interior corners (from index firstIdx on) with short fillet arcs, so the
// model rotates over several spline segments instead of snapping at the vertex.
bool SmoothCorners(Player* bot, Movement::PointsArray& path, size_t firstIdx, float radius, float arcThreshold,
                   float stepAngle, float maxDeviation)
{
    if (firstIdx < 1 || path.size() < firstIdx + 2)
        return false;

    Movement::PointsArray result(path.begin(), path.begin() + firstIdx);
    bool changed = false;
    int smoothed = 0;

    for (size_t i = firstIdx; i < path.size(); ++i)
    {
        G3D::Vector3 const b = path[i];
        if (i + 1 >= path.size() || smoothed >= MAX_SMOOTHED_CORNERS)
        {
            result.push_back(b);
            continue;
        }

        G3D::Vector3 const a = result.back();
        G3D::Vector3 const& c = path[i + 1];
        float const lenAB = Dist2d(a, b);
        float const lenBC = Dist2d(b, c);
        float const hIn = Bearing(a, b);
        float const hOut = Bearing(b, c);
        float const turn = SignedAngle(hOut - hIn);
        float const alpha = std::fabs(turn);

        // Hairpins (> 150 deg) are usually switchbacks on slopes; a fillet there would cut
        // across terrain the navmesh deliberately routed around.
        if (lenAB < 0.5f || lenBC < 0.5f || alpha < arcThreshold || alpha > PI_F * 5.0f / 6.0f)
        {
            result.push_back(b);
            continue;
        }

        // Navmesh paths bend exactly at obstacle corners, and a fillet cuts inside the corner.
        // Limit how far inside it may go: deviation of the arc midpoint = r * (1/cos(a/2) - 1).
        float const half = alpha * 0.5f;
        float r = radius;
        float const devPerRadius = 1.0f / std::cos(half) - 1.0f;
        if (devPerRadius > 0.0f)
            r = std::min(r, maxDeviation / devPerRadius);
        float t = r * std::tan(half);
        float const tMax = 0.45f * std::min(lenAB, lenBC);
        if (t > tMax)
        {
            t = tMax;
            r = t / std::tan(half);
        }
        if (t < 0.3f)
        {
            result.push_back(b);
            continue;
        }

        G3D::Vector3 const entry(b.x - std::cos(hIn) * t, b.y - std::sin(hIn) * t, b.z + (a.z - b.z) * (t / lenAB));
        G3D::Vector3 const exit(b.x + std::cos(hOut) * t, b.y + std::sin(hOut) * t, b.z + (c.z - b.z) * (t / lenBC));
        if (!HasClearSegment(bot, entry, exit))
        {
            result.push_back(b);
            continue;
        }

        float const dir = turn > 0.0f ? 1.0f : -1.0f;
        float const ox = entry.x + r * std::cos(hIn + dir * PI_F * 0.5f);
        float const oy = entry.y + r * std::sin(hIn + dir * PI_F * 0.5f);
        int const n = std::max(2, static_cast<int>(std::ceil(alpha / stepAngle)));

        result.push_back(entry);
        for (int k = 1; k < n; ++k)
        {
            float const f = static_cast<float>(k) / static_cast<float>(n);
            float const h = hIn + dir * alpha * f;
            float const px = ox + r * std::cos(h - dir * PI_F * 0.5f);
            float const py = oy + r * std::sin(h - dir * PI_F * 0.5f);
            float pz = entry.z + (exit.z - entry.z) * f;
            float const gz = bot->GetMapHeight(px, py, pz + 2.0f);
            if (gz > INVALID_HEIGHT && std::fabs(gz - pz) < 1.5f)
                pz = gz;
            result.push_back(G3D::Vector3(px, py, pz));
        }
        result.push_back(exit);

        changed = true;
        ++smoothed;
    }

    if (changed)
        path.swap(result);

    return changed;
}

// Duplicate or near-duplicate vertices produce zero-length spline segments, which the
// client renders as a facing glitch.
void RemoveTinySegments(Movement::PointsArray& path)
{
    Movement::PointsArray result;
    result.reserve(path.size());
    for (G3D::Vector3 const& p : path)
    {
        if (result.empty() || (p - result.back()).squaredLength() > 0.01f)
            result.push_back(p);
    }
    // Keep the exact destination even if it collapsed onto the previous point.
    if (result.size() >= 2 && !path.empty() && result.back() != path.back())
        result.back() = path.back();
    path.swap(result);
}

void SendTurnPacket(Player* bot, uint16 opcode)
{
    WorldPacket data(opcode, 64);
    data << bot->GetPackGUID();
    bot->BuildMovementPacket(&data);
    bot->SendMessageToSet(&data, false);
}

float CurrentTurnOrientation(LastMovement const& lm)
{
    float const elapsed = static_cast<float>(GetMSTimeDiffToNow(lm.turnStartMs)) / 1000.0f;
    float const turned = std::min(std::fabs(lm.turnAngle), elapsed * sPlayerbotAIConfig.realisticTurningTurnRate);
    return Position::NormalizeOrientation(lm.turnFromOri + (lm.turnAngle >= 0.0f ? turned : -turned));
}

void StopTurn(Player* bot, LastMovement& lm, float orientation)
{
    lm.turnActive = false;
    if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_MASK_TURNING))
        return;

    // Observers have been rotating the model locally since START_TURN; STOP_TURN carries the
    // orientation they should settle on, so the server value must be updated first.
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_TURNING);
    bot->SetOrientation(orientation);
    SendTurnPacket(bot, MSG_MOVE_STOP_TURN);
}

LastMovement* CurrentTurn(Player* bot, uint32 token)
{
    if (!bot->IsInWorld())
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    LastMovement& lm = GetLastMovement(botAI);
    if (!lm.turnActive || lm.turnToken != token)
        return nullptr;

    return &lm;
}

void FinishTurn(Player* bot, uint32 token, Movement::PointsArray& path)
{
    LastMovement* lm = CurrentTurn(bot, token);
    if (!lm)
        return;

    // Anything that happened during the turn (aggro, CC, another generator taking over, a
    // teleport) makes the stored path stale; stop turning where the bot visibly is and let
    // the AI decide again on its next tick.
    bool const canContinue = bot->IsAlive() && !bot->IsInCombat() && bot->CanFreeMove() &&
                             !bot->HasUnitState(UNIT_STATE_NOT_MOVE) && !bot->IsMovementPreventedByCasting() &&
                             bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                             path.size() >= 2 && bot->GetExactDist(path[0].x, path[0].y, path[0].z) < 1.0f;

    if (!canContinue)
    {
        StopTurn(bot, *lm, CurrentTurnOrientation(*lm));
        return;
    }

    float const finalOri = Position::NormalizeOrientation(lm->turnFromOri + lm->turnAngle);
    StopTurn(bot, *lm, finalOri);
    // MoveSplineInit::Launch seeds the spline with the unit's orientation.
    bot->SetOrientation(finalOri);
    bot->GetMotionMaster()->MoveSplinePath(&path);
}

uint32 StartTurn(PlayerbotAI* botAI, Player* bot, float fromOri, float signedAngle, Movement::PointsArray path)
{
    PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
    LastMovement& lm = GetLastMovement(botAI);
    uint32 const token = ++lm.turnToken;
    lm.turnActive = true;
    lm.turnStartMs = getMSTime();
    lm.turnFromOri = fromOri;
    lm.turnAngle = signedAngle;

    uint32 const durationMs = static_cast<uint32>(std::fabs(signedAngle) / cfg.realisticTurningTurnRate * 1000.0f);

    if (cfg.realisticTurningInPlaceTurnMode == 2)
    {
        // Each SetFacingTo is a zero-length monster move, which the client applies instantly;
        // spacing them at the turn rate approximates a continuous rotation.
        int const n = std::max(1, static_cast<int>(std::ceil(std::fabs(signedAngle) / cfg.realisticTurningArcStepAngle)));
        uint32 const stepMs = durationMs / static_cast<uint32>(n);
        for (int k = 1; k <= n; ++k)
        {
            float const ori =
                Position::NormalizeOrientation(fromOri + signedAngle * static_cast<float>(k) / static_cast<float>(n));
            bot->m_Events.AddEventAtOffset(
                [bot, token, ori]()
                {
                    // SetFacingTo launches a spline, so it would cancel any movement another
                    // system (raid script, follow) started without going through CancelTurn.
                    if (CurrentTurn(bot, token) && !bot->isMoving() &&
                        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
                        bot->SetFacingTo(ori);
                },
                Milliseconds(stepMs * static_cast<uint32>(k - 1)));
        }
    }
    else
    {
        // Same packets a real client sends while A/D is held: observers rotate the model
        // locally at the unit's turn rate, so a whole turn costs two packets.
        bot->AddUnitMovementFlag(signedAngle > 0.0f ? MOVEMENTFLAG_LEFT : MOVEMENTFLAG_RIGHT);
        SendTurnPacket(bot, signedAngle > 0.0f ? MSG_MOVE_START_TURN_LEFT : MSG_MOVE_START_TURN_RIGHT);
    }

    bot->m_Events.AddEventAtOffset([bot, token, path = std::move(path)]() mutable { FinishTurn(bot, token, path); },
                                   Milliseconds(durationMs));

    return durationMs;
}
}  // namespace

namespace RealisticTurning
{
void CancelTurn(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot || !sPlayerbotAIConfig.realisticTurningEnable)
        return;

    LastMovement& lm = GetLastMovement(botAI);
    if (!lm.turnActive)
        return;

    ++lm.turnToken;
    StopTurn(bot, lm, CurrentTurnOrientation(lm));
}

bool TryMove(PlayerbotAI* botAI, Player* bot, float x, float y, float z, uint32& extraDelayMs)
{
    extraDelayMs = 0;

    PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
    if (!cfg.realisticTurningEnable || !botAI || !bot)
        return false;

    // In combat and PvP every millisecond of reaction matters more than looks (fleeing, kiting,
    // positioning out of AoE), and raid scripts check for POINT_MOTION_TYPE.
    if (bot->IsInCombat() || bot->InBattleground() || bot->InArena())
        return false;

    // Vehicles, transports and taxis use other coordinate spaces or fixed splines; swimming
    // and flying moves are 3D straight splines where a ground arc makes no sense.
    if (bot->GetVehicle() || bot->GetTransport() || bot->IsInFlight() || !bot->CanFreeMove() || bot->IsFlying() ||
        bot->isSwimming() || bot->IsInWater())
        return false;

    // EscortMovementGenerator (used below) does not halt the spline on stun/root/cast the way
    // PointMovementGenerator does, so never hand it a unit that is already restricted.
    if (bot->HasUnitState(UNIT_STATE_NOT_MOVE) || bot->IsMovementPreventedByCasting())
        return false;

    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    // Short hops are formation adjustments and follow corrections; turning first would make
    // the bot lag behind whatever it is keeping up with.
    if (std::hypot(x - start.x, y - start.y) < cfg.realisticTurningMinMoveDistance)
        return false;

    // Nobody can see the difference without a real player nearby, and with thousands of bots the
    // extra path/LOS work is not free.
    if (cfg.realisticTurningOnlyNearPlayers && !botAI->HasPlayerNearby(bot->GetMap()->GetVisibilityRange()))
        return false;

    // Same query PointMovementGenerator would run, done here so the path can be modified.
    PathGenerator gen(bot);
    if (!gen.CalculatePath(x, y, z, false) || (gen.GetPathType() & PATHFIND_NOPATH))
        return false;

    Movement::PointsArray path = gen.GetPath();
    if (path.size() < 2)
        return false;
    path[0] = start;

    float const rate = cfg.realisticTurningTurnRate;
    float const speed = bot->GetSpeed(bot->HasUnitMovementFlag(MOVEMENTFLAG_WALKING) ? MOVE_WALK : MOVE_RUN);
    // Moving at v while rotating at w traces a circle of radius v / w.
    float const radius = std::clamp(speed / rate, 0.5f, 8.0f);

    // Aim at the first path point clearly outside the turning circle: points closer than that
    // cannot be reached by a smooth turn anyway and would dominate the heading with navmesh noise.
    size_t joinIdx = path.size() - 1;
    for (size_t i = 1; i < path.size(); ++i)
    {
        if (Dist2d(start, path[i]) > 2.0f * radius)
        {
            joinIdx = i;
            break;
        }
    }
    G3D::Vector3 const target = path[joinIdx];

    float const heading = bot->GetOrientation();
    float const delta = SignedAngle(Bearing(start, target) - heading);
    float const absDelta = std::fabs(delta);
    float const dir = delta >= 0.0f ? 1.0f : -1.0f;
    float const stepAngle = cfg.realisticTurningArcStepAngle;

    // A running player redirecting never stops to rotate on the spot; only standing bots do.
    bool const standing = bot->movespline->Finalized() && !bot->isMoving();
    bool const inPlaceAllowed = cfg.realisticTurningInPlaceTurnMode != 0 && standing;

    Movement::PointsArray newPath;
    newPath.reserve(path.size() + 16);
    newPath.push_back(start);
    size_t restIdx = 1;
    float turnSigned = 0.0f;  // in-place rotation before moving, positive = left
    bool changed = false;

    if (absDelta >= cfg.realisticTurningArcAngleThreshold)
    {
        float arcHeading = heading;
        if (inPlaceAllowed && absDelta >= cfg.realisticTurningInPlaceTurnThreshold)
        {
            // Rotate on the spot for most of it and keep the last ArcAngleThreshold for a
            // running curve, like tapping A/D and then steering with the mouse.
            turnSigned = dir * (absDelta - cfg.realisticTurningArcAngleThreshold);
            arcHeading = heading + turnSigned;
        }

        Movement::PointsArray arc;
        if (BuildLeadIn(bot, start, arcHeading, dir, radius, stepAngle, absDelta - std::fabs(turnSigned), target,
                        arc) &&
            HasClearSegment(bot, arc.empty() ? start : arc.back(), target))
        {
            newPath.insert(newPath.end(), arc.begin(), arc.end());
            restIdx = joinIdx;
            changed = !arc.empty();
        }
        else if (inPlaceAllowed)
        {
            // Target inside the turning circle, or the curve would leave walkable ground:
            // face the first real segment on the spot, then walk the path unchanged. Vertices
            // within a yard of the bot are navmesh snapping noise, not a direction.
            size_t faceIdx = path.size() - 1;
            for (size_t i = 1; i < path.size(); ++i)
            {
                if (Dist2d(start, path[i]) > 1.0f)
                {
                    faceIdx = i;
                    break;
                }
            }
            turnSigned = SignedAngle(Bearing(start, path[faceIdx]) - heading);
            restIdx = 1;
        }
        else
        {
            // Running and the curve is not walkable: keep the upstream snap rather than risk
            // walking a bot into a wall.
            turnSigned = 0.0f;
            restIdx = 1;
        }
    }

    size_t const firstRestIdx = newPath.size();
    newPath.insert(newPath.end(), path.begin() + restIdx, path.end());

    if (cfg.realisticTurningSmoothCorners)
        changed |= SmoothCorners(bot, newPath, firstRestIdx, radius, cfg.realisticTurningArcAngleThreshold, stepAngle,
                                 cfg.realisticTurningCornerMaxDeviation);

    bool const turnFirst = std::fabs(turnSigned) >= MIN_IN_PLACE_TURN;
    if (!changed && !turnFirst)
        return false;

    RemoveTinySegments(newPath);
    if (newPath.size() < 2)
        return false;

    bot->GetMotionMaster()->Clear();

    if (turnFirst)
    {
        extraDelayMs = StartTurn(botAI, bot, heading, turnSigned, std::move(newPath));
        return true;
    }

    bot->GetMotionMaster()->MoveSplinePath(&newPath);
    return true;
}
}  // namespace RealisticTurning
