#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <set>
#include "BlockerPasses.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "module.h"

BlockerPasses g_BlockerPasses;
PLUGIN_EXPOSE(BlockerPasses, g_BlockerPasses);

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;
extern ISource2Server* g_pSource2Server;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IMenusApi* g_pMenus = nullptr;
IAdminApi* g_pAdmin = nullptr;

typedef void (*SetCollisionBounds_t)(CBaseEntity*, const Vector*, const Vector*);
static SetCollisionBounds_t g_fnSetCollisionBounds = nullptr;

struct ModelDef
{
    std::string label;
    std::string path;
};

struct BPItem
{
    std::string label;
    std::string path;
    Vector pos;
    QAngle ang;
    float scale = 1.0f;
    bool invisible = false;
    bool isWall = false;
    Vector pos2;
    int beamR = 0;
    int beamG = 128;
    int beamB = 255;
    bool beamRainbow = false;
    float wallYaw = 0.0f;
    int itemR = 255;
    int itemG = 255;
    int itemB = 255;
};

struct LiveEnt
{
    int index;
    CHandle<CBaseEntity> ent;
    std::vector<CHandle<CBaseEntity>> beams;
    std::vector<CHandle<CBaseEntity>> wallColls;
};

static std::vector<ModelDef> g_ModelDefs;
static std::vector<BPItem>   g_Items;
static std::vector<LiveEnt>  g_Live;

static std::string g_CurrentMap;

enum PingMode
{
    PING_NONE = 0,
    PING_TELEPORT = 1,
    PING_WALL_POS1 = 2,
    PING_WALL_POS2 = 3
};

static PingMode g_ePingMode[64];
static int      g_iPingTargetIndex[64];
static Vector   g_vWallTempPos[64];

static int g_MinPlayersToOpen = 10;
static bool g_DebugLog = true;
static bool g_bIgnoreSpectators = true;
static std::string g_AccessPermission = "@admin/bp";
static std::string g_AccessFlag = "";

static float g_flRainbowHue = 0.0f;
static bool  g_bRainbowTimerActive = false;

static std::set<uint64_t> g_TempAccessSteamIDs;

static std::map<std::string, std::string> g_Phrases;

static inline const char* Phrase(const char* key, const char* def = "")
{
    auto it = g_Phrases.find(key);
    return (it != g_Phrases.end() && !it->second.empty()) ? it->second.c_str() : def;
}

static void LoadPhrases()
{
    g_Phrases.clear();
    KeyValues::AutoDelete kv("Phrases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/translations/blockerpasses.phrases.txt"))
    {
        return;
    }

    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues* p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
    {
        g_Phrases[p->GetName()] = p->GetString(lang);
    }
}

static inline void PrintChatRaw(int slot, const char* fmt, va_list va)
{
    if (!g_pUtils)
    {
        return;
    }
    char buf[1024];
    V_vsnprintf(buf, sizeof(buf), fmt, va);

    const char* tag = Phrase("Chat_Prefix", "");
    if (*tag)
    {
        std::string out = " " + std::string(tag) + " " + std::string(buf);
        g_pUtils->PrintToChat(slot, "%s", out.c_str());
    }
    else
    {
        g_pUtils->PrintToChat(slot, "%s", buf);
    }
}

static inline void PrintChat(int slot, const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    PrintChatRaw(slot, fmt, va);
    va_end(va);
}

static inline void PrintChatKey(int slot, const char* key, const char* fallbackFmt, ...)
{
    const char* fmt = Phrase(key, fallbackFmt);
    va_list va;
    va_start(va, fallbackFmt);
    PrintChatRaw(slot, fmt, va);
    va_end(va);
}

static inline void PrintChatAllKey(const char* key, const char* fallbackFmt, ...)
{
    if (!g_pUtils)
    {
        return;
    }
    const char* fmt = Phrase(key, fallbackFmt);
    char msg[1024];
    va_list va;
    va_start(va, fallbackFmt);
    V_vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);

    const char* tag = Phrase("Chat_Prefix", "");
    if (*tag)
    {
        std::string out = " " + std::string(tag) + " " + std::string(msg);
        g_pUtils->PrintToChatAll("%s", out.c_str());
    }
    else
    {
        g_pUtils->PrintToChatAll("%s", msg);
    }
}

static std::string NormalizeMapName(const char* in)
{
    if (!in || !*in)
    {
        return "";
    }
    std::string s(in);

    size_t pipePos = s.find(" | ");
    if (pipePos != std::string::npos)
    {
        s = s.substr(0, pipePos);
    }

    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t pos = s.find_last_of('/');
    if (pos != std::string::npos)
    {
        s = s.substr(pos + 1);
    }
    pos = s.find_last_of('.');
    if (pos != std::string::npos)
    {
        s = s.substr(0, pos);
    }
    return s;
}

static inline int HumansOnline()
{
    int c = 0;
    for (int i = 0; i < 64; ++i)
    {
        if (!g_pPlayers->IsInGame(i) || g_pPlayers->IsFakeClient(i))
        {
            continue;
        }
        if (g_bIgnoreSpectators)
        {
            CCSPlayerController* pc = CCSPlayerController::FromSlot(i);
            if (pc && pc->m_iTeamNum() <= 1)
            {
                continue;
            }
        }
        ++c;
    }
    return c;
}

static inline bool ShouldBeOpen()
{
    return HumansOnline() >= g_MinPlayersToOpen;
}

static void Dbg(const char* fmt, ...)
{
    if (!g_DebugLog)
    {
        return;
    }
    char buf[1024];
    va_list va;
    va_start(va, fmt);
    V_vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    ConColorMsg(Color(150, 200, 255, 255), "[BlockerPasses] %s\n", buf);
}

static void KillWallCollision(CBaseEntity* ent);

static void ClearLive(bool removeEntities)
{
    if (removeEntities)
    {
        for (auto& le : g_Live)
        {
            bool isWall = (le.index >= 0 && le.index < (int)g_Items.size() && g_Items[le.index].isWall);
            if (isWall)
            {
                for (auto& wc : le.wallColls)
                {
                    if (wc.Get())
                    {
                        KillWallCollision(wc.Get());
                    }
                }
                le.wallColls.clear();
            }
            else if (le.ent.Get())
            {
                g_pUtils->RemoveEntity((CEntityInstance*)le.ent.Get());
            }
            for (auto& bh : le.beams)
            {
                if (bh.Get())
                {
                    g_pUtils->RemoveEntity((CEntityInstance*)bh.Get());
                }
            }
        }
    }
    g_Live.clear();
    g_bRainbowTimerActive = false;
}

static inline void RemoveLiveBeams(LiveEnt& le)
{
    for (auto& bh : le.beams)
    {
        if (bh.Get())
        {
            g_pUtils->RemoveEntity((CEntityInstance*)bh.Get());
        }
    }
    le.beams.clear();
}

static void KillWallCollision(CBaseEntity* ent)
{
    if (!ent)
    {
        return;
    }
    auto* me = dynamic_cast<CBaseModelEntity*>(ent);
    if (me)
    {
        me->m_Collision().m_nSolidType() = SOLID_NONE;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_nSolidType");

        Vector zero(0, 0, 0);
        me->m_Collision().m_vecMins() = zero;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecMins");
        me->m_Collision().m_vecMaxs() = zero;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecMaxs");
        me->m_Collision().m_vecSpecifiedSurroundingMins() = zero;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecSpecifiedSurroundingMins");
        me->m_Collision().m_vecSpecifiedSurroundingMaxs() = zero;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecSpecifiedSurroundingMaxs");

        if (g_fnSetCollisionBounds)
        {
            g_fnSetCollisionBounds(ent, &zero, &zero);
        }
    }

    Vector voidPos(0, 0, -15000);
    QAngle noAng(0, 0, 0);
    g_pUtils->TeleportEntity(ent, &voidPos, &noAng, nullptr);

    g_pUtils->RemoveEntity((CEntityInstance*)ent);
}

static inline float ClampScale(float v)
{
    if (v < 0.05f)
    {
        v = 0.05f;
    }
    if (v > 20.0f)
    {
        v = 20.0f;
    }
    return v;
}

static inline void ApplyRenderAlpha(CBaseModelEntity* ent, uint8_t a)
{
    if (!ent)
    {
        return;
    }
    if (ent->m_clrRender().a() == a)
    {
        return;
    }
    Color cur = ent->m_clrRender();
    ent->m_clrRender() = Color(cur.r(), cur.g(), cur.b(), a);
    g_pUtils->SetStateChanged(ent, "CBaseModelEntity", "m_clrRender");
}

static inline void ApplyRenderColor(CBaseModelEntity* ent, int r, int g, int b)
{
    if (!ent)
    {
        return;
    }
    uint8_t a = ent->m_clrRender().a();
    ent->m_clrRender() = Color(r, g, b, a);
    g_pUtils->SetStateChanged(ent, "CBaseModelEntity", "m_clrRender");
}

static inline void SetNoDraw(CBaseEntity* ent, bool on)
{
    if (!ent)
    {
        return;
    }
    int fx = ent->m_fEffects();
    int nf = on ? (fx | EF_NODRAW) : (fx & ~EF_NODRAW);
    if (nf == fx)
    {
        return;
    }
    ent->m_fEffects() = nf;
    g_pUtils->SetStateChanged(ent, "CBaseEntity", "m_fEffects");
}

static void HueToRGB(float hue, int& r, int& g, int& b)
{
    float h = fmodf(hue, 360.0f) / 60.0f;
    int i = (int)h;
    float f = h - i;
    int v = 255;
    int q = (int)(255 * (1.0f - f));
    int t = (int)(255 * f);
    switch (i % 6)
    {
        case 0: r = v; g = t; b = 0; break;
        case 1: r = q; g = v; b = 0; break;
        case 2: r = 0; g = v; b = t; break;
        case 3: r = 0; g = q; b = v; break;
        case 4: r = t; g = 0; b = v; break;
        case 5: r = v; g = 0; b = q; break;
    }
}

static CBaseEntity* CreateBeamLine(const Vector& start, const Vector& end, int cr, int cg, int cb, float width = 1.0f)
{
    CBaseEntity* ent = (CBaseEntity*)g_pUtils->CreateEntityByName("env_beam", CEntityIndex(-1));
    if (!ent)
    {
        Dbg("CreateBeamLine: CreateEntityByName failed");
        return nullptr;
    }

    char colorStr[32];
    V_snprintf(colorStr, sizeof(colorStr), "%d %d %d", cr, cg, cb);

    CEntityKeyValues* kv = new CEntityKeyValues();
    kv->SetFloat("BoltWidth", width);
    kv->SetString("rendercolor", colorStr);
    kv->SetInt("renderamt", 255);
    kv->SetFloat("life", 0.0f);
    g_pUtils->DispatchSpawn((CEntityInstance*)ent, kv);

    QAngle noAng(0, 0, 0);
    g_pUtils->TeleportEntity(ent, &start, &noAng, nullptr);

    CBeam* beam = (CBeam*)ent;
    beam->m_vecEndPos() = end;
    g_pUtils->SetStateChanged(ent, "CBeam", "m_vecEndPos");

    beam->m_fWidth() = width;
    g_pUtils->SetStateChanged(ent, "CBeam", "m_fWidth");

    Dbg("CreateBeamLine: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)", start.x, start.y, start.z, end.x, end.y, end.z);
    return ent;
}

static std::vector<CHandle<CBaseEntity>> DrawWireframe(const Vector& p1, const Vector& p2, int bR, int bG, int bB, bool rainbow, float yaw = 0.0f, float width = 1.0f)
{
    float minX = fminf(p1.x, p2.x), maxX = fmaxf(p1.x, p2.x);
    float minY = fminf(p1.y, p2.y), maxY = fmaxf(p1.y, p2.y);
    float minZ = fminf(p1.z, p2.z), maxZ = fmaxf(p1.z, p2.z);

    Vector c[8] = {
        {minX, minY, minZ}, {maxX, minY, minZ}, {maxX, maxY, minZ}, {minX, maxY, minZ},
        {minX, minY, maxZ}, {maxX, minY, maxZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ}
    };

    if (yaw != 0.0f)
    {
        float cx = (minX + maxX) * 0.5f;
        float cy = (minY + maxY) * 0.5f;
        float rad = yaw * (float)M_PI / 180.0f;
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        for (int i = 0; i < 8; ++i)
        {
            float dx = c[i].x - cx;
            float dy = c[i].y - cy;
            c[i].x = cx + dx * cosA - dy * sinA;
            c[i].y = cy + dx * sinA + dy * cosA;
        }
    }

    int edges[][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    int crossEdges[12][2] = {
        {0, 2}, {1, 3},
        {4, 6}, {5, 7},
        {0, 5}, {1, 4},
        {3, 6}, {2, 7},
        {0, 7}, {3, 4},
        {1, 6}, {2, 5}
    };

    std::vector<CHandle<CBaseEntity>> beams;
    int edgeIdx = 0;
    for (auto& e : edges)
    {
        int cr, cg, cb;
        if (rainbow)
        {
            HueToRGB(g_flRainbowHue, cr, cg, cb);
        }
        else
        {
            cr = bR;
            cg = bG;
            cb = bB;
        }

        CBaseEntity* b = CreateBeamLine(c[e[0]], c[e[1]], cr, cg, cb, width);
        if (b)
        {
            beams.push_back(CHandle<CBaseEntity>(b));
        }
        edgeIdx++;
    }
    for (int i = 0; i < 12; ++i)
    {
        int cr, cg, cb;
        if (rainbow)
        {
            HueToRGB(g_flRainbowHue, cr, cg, cb);
        }
        else
        {
            cr = bR;
            cg = bG;
            cb = bB;
        }
        CBaseEntity* b = CreateBeamLine(c[crossEdges[i][0]], c[crossEdges[i][1]], cr, cg, cb, width);
        if (b)
        {
            beams.push_back(CHandle<CBaseEntity>(b));
        }
    }
    return beams;
}

static void StartRainbowTimer()
{
    if (g_bRainbowTimerActive)
    {
        return;
    }
    g_bRainbowTimerActive = true;
    g_pUtils->CreateTimer(0.1f, []() -> float {
        g_flRainbowHue += 10.0f;
        if (g_flRainbowHue >= 360.0f)
        {
            g_flRainbowHue -= 360.0f;
        }

        bool anyRainbow = false;
        for (auto& le : g_Live)
        {
            if (le.index < 0 || le.index >= (int)g_Items.size())
            {
                continue;
            }
            if (!g_Items[le.index].isWall || !g_Items[le.index].beamRainbow)
            {
                continue;
            }
            anyRainbow = true;
            int rv, gv, bv;
            HueToRGB(g_flRainbowHue, rv, gv, bv);
            for (auto& bh : le.beams)
            {
                CBaseEntity* beam = bh.Get();
                if (beam)
                {
                    auto* me = dynamic_cast<CBaseModelEntity*>(beam);
                    if (me)
                    {
                        me->m_clrRender() = Color(rv, gv, bv, 255);
                        g_pUtils->SetStateChanged(me, "CBaseModelEntity", "m_clrRender");
                    }
                }
            }
        }

        if (!anyRainbow)
        {
            g_bRainbowTimerActive = false;
            return -1.0f;
        }
        return 0.1f;
    });
}

static CBaseEntity* SpawnOneCollisionBox(const Vector& boxCenter, const Vector& vmins, const Vector& vmaxs, float yaw = 0.0f)
{
    CBaseEntity* ent = (CBaseEntity*)g_pUtils->CreateEntityByName("func_brush", CEntityIndex(-1));
    if (!ent)
    {
        Dbg("SpawnOneCollisionBox: CreateEntityByName func_brush failed");
        return nullptr;
    }

    auto* me = dynamic_cast<CBaseModelEntity*>(ent);
    if (me)
    {
        me->m_nRenderMode() = kRenderNone;
        g_pUtils->SetStateChanged(me, "CBaseModelEntity", "m_nRenderMode");
    }

    QAngle ang(0, yaw, 0);
    g_pUtils->TeleportEntity(ent, &boxCenter, &ang, nullptr);

    g_pUtils->DispatchSpawn((CEntityInstance*)ent, nullptr);

    float cosAbs = fabsf(cosf(yaw * (float)M_PI / 180.0f));
    float sinAbs = fabsf(sinf(yaw * (float)M_PI / 180.0f));
    float surroundHX = fabsf(vmaxs.x) * cosAbs + fabsf(vmaxs.y) * sinAbs;
    float surroundHY = fabsf(vmaxs.x) * sinAbs + fabsf(vmaxs.y) * cosAbs;
    Vector surroundMins(-surroundHX, -surroundHY, vmins.z);
    Vector surroundMaxs(surroundHX, surroundHY, vmaxs.z);

    if (me)
    {
        me->m_Collision().m_nSurroundType() = 3;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_nSurroundType");

        me->m_Collision().m_vecSpecifiedSurroundingMaxs() = surroundMaxs;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecSpecifiedSurroundingMaxs");

        me->m_Collision().m_vecSpecifiedSurroundingMins() = surroundMins;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecSpecifiedSurroundingMins");

        me->m_Collision().m_vecMins() = vmins;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecMins");

        me->m_Collision().m_vecMaxs() = vmaxs;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_vecMaxs");

        me->m_Collision().m_collisionAttribute().m_nCollisionGroup() = 0;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_collisionAttribute");

        me->m_Collision().m_CollisionGroup() = 0;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_CollisionGroup");

        me->m_Collision().m_nSolidType() = SOLID_OBB;
        g_pUtils->SetStateChanged(me, "CCollisionProperty", "m_nSolidType");
    }

    if (me)
    {
        me->m_clrRender() = Color(0, 0, 0, 0);
        g_pUtils->SetStateChanged(me, "CBaseModelEntity", "m_clrRender");
    }

    CHandle<CBaseEntity> hEnt(ent);
    Vector capturedMins = vmins;
    Vector capturedMaxs = vmaxs;

    g_pUtils->CreateTimer(0.0f, [hEnt, capturedMins, capturedMaxs]() -> float {
        CBaseEntity* e = hEnt.Get();
        if (!e)
        {
            return -1.0f;
        }

        g_pUtils->SetEntityModel((CBaseModelEntity*)e,
            "models/props/de_dust/hr_dust/dust_soccerball/dust_soccer_ball001.vmdl");

        Vector mins = capturedMins;
        Vector maxs = capturedMaxs;
        g_fnSetCollisionBounds(e, &mins, &maxs);

        Dbg("SpawnOneCollisionBox: deferred SetCollisionBounds applied (OBB)");
        return -1.0f;
    });

    return ent;
}

static std::vector<CBaseEntity*> SpawnWallCollisions(const Vector& p1, const Vector& p2, float yaw = 0.0f)
{
    std::vector<CBaseEntity*> result;

    if (!g_fnSetCollisionBounds)
    {
        Dbg("SpawnWallCollisions: SetCollisionBounds not found");
        return result;
    }

    Vector center;
    float halfExt[3];
    for (int a = 0; a < 3; ++a)
    {
        float lo = fminf(p1[a], p2[a]);
        float hi = fmaxf(p1[a], p2[a]);
        center[a] = (lo + hi) * 0.5f;
        halfExt[a] = fmaxf((hi - lo) * 0.5f, 1.0f);
    }

    float normalYaw = fmodf(yaw, 360.0f);
    if (normalYaw < 0.0f)
    {
        normalYaw += 360.0f;
    }

    bool isAxisAligned = (normalYaw < 1.0f || fabsf(normalYaw - 90.0f) < 1.0f ||
                          fabsf(normalYaw - 180.0f) < 1.0f || fabsf(normalYaw - 270.0f) < 1.0f ||
                          normalYaw > 359.0f);

    if (isAxisAligned)
    {
        float hx = halfExt[0], hy = halfExt[1];
        if (fabsf(normalYaw - 90.0f) < 1.0f || fabsf(normalYaw - 270.0f) < 1.0f)
        {
            float tmp = hx;
            hx = hy;
            hy = tmp;
        }
        Vector vmins(-hx, -hy, -halfExt[2]);
        Vector vmaxs(hx, hy, halfExt[2]);
        CBaseEntity* ent = SpawnOneCollisionBox(center, vmins, vmaxs);
        if (ent)
        {
            result.push_back(ent);
        }
        Dbg("SpawnWallCollisions: axis-aligned yaw=%.1f box at (%.1f %.1f %.1f)", yaw, center.x, center.y, center.z);
    }
    else
    {
        float halfX = halfExt[0];
        float halfY = halfExt[1];
        float halfZ = halfExt[2];

        Vector vmins(-halfX, -halfY, -halfZ);
        Vector vmaxs(halfX, halfY, halfZ);
        CBaseEntity* ent = SpawnOneCollisionBox(center, vmins, vmaxs, yaw);
        if (ent)
        {
            result.push_back(ent);
        }

        Dbg("SpawnWallCollisions: OBB yaw=%.1f center(%.1f %.1f %.1f) half(%.1f %.1f %.1f)",
            yaw, center.x, center.y, center.z, halfX, halfY, halfZ);
    }

    return result;
}

static void SpawnLiveEntry(int index, LiveEnt& le)
{
    const BPItem& it = g_Items[index];
    if (it.isWall)
    {
        auto wallEnts = SpawnWallCollisions(it.pos, it.pos2, it.wallYaw);
        for (auto* e : wallEnts)
        {
            le.wallColls.push_back(CHandle<CBaseEntity>(e));
        }
        if (!wallEnts.empty())
        {
            le.ent = CHandle<CBaseEntity>(wallEnts[0]);
        }
        le.beams = DrawWireframe(it.pos, it.pos2, it.beamR, it.beamG, it.beamB, it.beamRainbow, it.wallYaw);
        if (it.beamRainbow)
        {
            StartRainbowTimer();
        }
    }
}

static CBaseEntity* SpawnOne(const BPItem& it)
{
    if (it.path.empty())
    {
        Dbg("SpawnOne: empty path");
        return nullptr;
    }
    if (!strstr(it.path.c_str(), ".vmdl"))
    {
        Dbg("SpawnOne: invalid model '%s'", it.path.c_str());
        return nullptr;
    }

    const char* classes[] = { "prop_dynamic", "prop_dynamic_override" };
    for (const char* cls : classes)
    {
        CBaseEntity* ent = (CBaseEntity*)g_pUtils->CreateEntityByName(cls, CEntityIndex(-1));
        if (!ent)
        {
            Dbg("SpawnOne: CreateEntityByName failed for %s", cls);
            continue;
        }

        CEntityKeyValues* kv = new CEntityKeyValues();
        kv->SetString("model", it.path.c_str());
        kv->SetInt("solid", 6);
        kv->SetInt("DisableBoneFollowers", 1);

        float safeScale = ClampScale(it.scale);
        kv->SetFloat("uniformscale", safeScale);

        g_pUtils->DispatchSpawn((CEntityInstance*)ent, kv);
        g_pUtils->TeleportEntity((CBaseEntity*)ent, &it.pos, &it.ang, nullptr);

        if (it.invisible)
        {
            auto* me = dynamic_cast<CBaseModelEntity*>(ent);
            ApplyRenderAlpha(me, 0);
            SetNoDraw(ent, true);
        }

        if (it.itemR != 255 || it.itemG != 255 || it.itemB != 255)
        {
            auto* me = dynamic_cast<CBaseModelEntity*>(ent);
            ApplyRenderColor(me, it.itemR, it.itemG, it.itemB);
        }

        Dbg("SpawnOne: %s '%s' at (%.1f %.1f %.1f) ang(%.1f %.1f %.1f) scale=%.3f invis=%d",
            cls, it.path.c_str(), it.pos.x, it.pos.y, it.pos.z, it.ang.x, it.ang.y, it.ang.z, safeScale, (int)it.invisible);
        return ent;
    }
    Dbg("SpawnOne: failed model '%s'", it.path.c_str());
    return nullptr;
}

static void EnsureClosed()
{
    std::vector<int> aliveIdx;
    for (auto& le : g_Live)
    {
        aliveIdx.push_back(le.index);
    }

    for (int i = 0; i < (int)g_Items.size(); ++i)
    {
        if (std::find(aliveIdx.begin(), aliveIdx.end(), i) != aliveIdx.end())
        {
            continue;
        }

        if (g_Items[i].isWall)
        {
            LiveEnt le;
            le.index = i;
            SpawnLiveEntry(i, le);
            g_Live.push_back(std::move(le));
        }
        else
        {
            CBaseEntity* e = SpawnOne(g_Items[i]);
            if (e)
            {
                LiveEnt le;
                le.index = i;
                le.ent = CHandle<CBaseEntity>(e);
                g_Live.push_back(std::move(le));
            }
            else
            {
                Dbg("EnsureClosed: spawn failed for %d", i);
            }
        }
    }
}

static void EnsureOpen()
{
    ClearLive(true);
}

static void ApplyState()
{
    if (ShouldBeOpen())
    {
        EnsureOpen();
    }
    else
    {
        EnsureClosed();
    }
}

static void SaveData()
{
    const char* path = "addons/data/bp_data.ini";

    KeyValues::AutoDelete root("BPData");
    root->LoadFromFile(g_pFullFileSystem, path);

    if (KeyValues* old = root->FindKey(g_CurrentMap.c_str(), false))
    {
        root->RemoveSubKey(old);
    }

    KeyValues* mapKV = root->FindKey(g_CurrentMap.c_str(), true);
    for (int i = 0; i < (int)g_Items.size(); ++i)
    {
        const BPItem& it = g_Items[i];
        KeyValues* k = mapKV->CreateNewKey();
        k->SetName("item");
        k->SetString("label", it.label.c_str());
        k->SetString("path", it.path.c_str());
        k->SetFloat("px", it.pos.x);
        k->SetFloat("py", it.pos.y);
        k->SetFloat("pz", it.pos.z);
        k->SetFloat("ax", it.ang.x);
        k->SetFloat("ay", it.ang.y);
        k->SetFloat("az", it.ang.z);
        k->SetFloat("sc", it.scale);
        k->SetInt("iv", it.invisible ? 1 : 0);
        k->SetInt("wall", it.isWall ? 1 : 0);
        if (it.isWall)
        {
            k->SetFloat("p2x", it.pos2.x);
            k->SetFloat("p2y", it.pos2.y);
            k->SetFloat("p2z", it.pos2.z);
            k->SetInt("br", it.beamR);
            k->SetInt("bg", it.beamG);
            k->SetInt("bb", it.beamB);
            k->SetInt("brb", it.beamRainbow ? 1 : 0);
            if (it.wallYaw != 0.0f)
            {
                k->SetFloat("wy", it.wallYaw);
            }
        }
        if (!it.isWall)
        {
            k->SetInt("ir", it.itemR);
            k->SetInt("ig", it.itemG);
            k->SetInt("ib", it.itemB);
        }
    }
    root->SaveToFile(g_pFullFileSystem, path);
    Dbg("Saved %d items for map %s", (int)g_Items.size(), g_CurrentMap.c_str());
}

static void LoadDataForMap(const char* map)
{
    if (map && *map)
    {
        g_CurrentMap = NormalizeMapName(map);
    }

    g_Items.clear();
    ClearLive(true);

    KeyValues::AutoDelete root("BPData");
    if (!root->LoadFromFile(g_pFullFileSystem, "addons/data/bp_data.ini"))
    {
        Dbg("No data file yet for map %s", g_CurrentMap.c_str());
        return;
    }
    KeyValues* mapKV = root->FindKey(g_CurrentMap.c_str(), false);
    if (!mapKV)
    {
        Dbg("No section for map %s", g_CurrentMap.c_str());
        return;
    }

    for (KeyValues* k = mapKV->GetFirstTrueSubKey(); k; k = k->GetNextTrueSubKey())
    {
        BPItem it;
        it.label = k->GetString("label", "");
        it.path = k->GetString("path", "");
        it.pos.x = k->GetFloat("px", 0.f);
        it.pos.y = k->GetFloat("py", 0.f);
        it.pos.z = k->GetFloat("pz", 0.f);
        it.ang.x = k->GetFloat("ax", 0.f);
        it.ang.y = k->GetFloat("ay", 0.f);
        it.ang.z = k->GetFloat("az", 0.f);
        it.scale = k->GetFloat("sc", 1.0f);
        it.invisible = k->GetInt("iv", 0) != 0;
        it.isWall = k->GetInt("wall", 0) != 0;
        if (it.isWall)
        {
            it.pos2.x = k->GetFloat("p2x", 0.f);
            it.pos2.y = k->GetFloat("p2y", 0.f);
            it.pos2.z = k->GetFloat("p2z", 0.f);
            it.beamR = k->GetInt("br", 0);
            it.beamG = k->GetInt("bg", 128);
            it.beamB = k->GetInt("bb", 255);
            it.beamRainbow = k->GetInt("brb", 0) != 0;
            it.wallYaw = k->GetFloat("wy", 0.0f);
        }
        if (!it.isWall)
        {
            it.itemR = k->GetInt("ir", 255);
            it.itemG = k->GetInt("ig", 255);
            it.itemB = k->GetInt("ib", 255);
        }
        if (!it.path.empty() || it.isWall)
        {
            g_Items.push_back(std::move(it));
        }
    }
    Dbg("Loaded %d items for map %s", (int)g_Items.size(), g_CurrentMap.c_str());
}

static void LoadSettings()
{
    KeyValues::AutoDelete kv("BlockerPasses");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/BlockerPasses/settings.ini"))
    {
        g_MinPlayersToOpen = 10;
        g_AccessPermission = "@admin/bp";
        g_AccessFlag = "";
        g_DebugLog = true;
        g_bIgnoreSpectators = true;

        g_ModelDefs.clear();
        g_ModelDefs.push_back({"Желзеные двери", "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"});
        g_ModelDefs.push_back({"Желзеный забор", "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"});
        Dbg("Settings not found, using defaults");
        return;
    }

    g_MinPlayersToOpen = kv->GetInt("min_players_to_open", 10);
    g_AccessPermission = kv->GetString("access_permission", "@admin/bp");
    g_AccessFlag = kv->GetString("access_flag", "");
    g_DebugLog = kv->GetInt("debug_log", 1) != 0;
    g_bIgnoreSpectators = kv->GetInt("ignore_spectators", 1) != 0;

    g_ModelDefs.clear();
    if (KeyValues* models = kv->FindKey("models", false))
    {
        for (KeyValues* m = models->GetFirstTrueSubKey(); m; m = m->GetNextTrueSubKey())
        {
            const char* label = m->GetString("label", "");
            const char* path = m->GetString("path", "");
            if (path && *path)
            {
                ModelDef md;
                md.path = path;
                md.label = (label && *label) ? label : path;
                g_ModelDefs.push_back(std::move(md));
            }
        }
    }

    if (g_ModelDefs.empty())
    {
        Dbg("No models in settings.ini -> nothing to place");
    }

    Dbg("Settings: min_players_to_open=%d, debug=%d, perm='%s', flag='%s', models=%d",
        g_MinPlayersToOpen, (int)g_DebugLog, g_AccessPermission.c_str(), g_AccessFlag.c_str(), (int)g_ModelDefs.size());
}

static void OpenModelMenu(int slot);
static void OpenMainMenu(int slot);
static void OpenEditListMenu(int slot);
static void OpenItemMenu(int slot, int index);
static void OpenMoveMenu(int slot, int index);
static void OpenRotateSubMenu(int slot, int index);
static void OpenScaleMenu(int slot, int index);
static void OpenBeamColorMenu(int slot, int index);
static void OpenWallRotateMenu(int slot, int index);
static void OpenWallMoveMenu(int slot, int index);
static void OpenWallScaleMenu(int slot, int index);
static void OpenItemColorMenu(int slot, int index);

static int FindItemByCrosshair(int slot, float maxDist = 128.0f)
{
    trace_info_t tr = g_pPlayers->RayTrace(slot);
    Vector hit = tr.m_vEndPos;

    int best = -1;
    float bestSq = maxDist * maxDist;

    for (auto& le : g_Live)
    {
        int idx = le.index;
        if (idx < 0 || idx >= (int)g_Items.size())
        {
            continue;
        }
        Vector d = g_Items[idx].pos - hit;
        float dsq = d.x * d.x + d.y * d.y + d.z * d.z;
        if (dsq <= bestSq)
        {
            bestSq = dsq;
            best = idx;
        }
    }
    return best;
}

static void EnsureCorrectMapLoaded()
{
    CGlobalVars* gv = g_pUtils->GetCGlobalVars();
    if (!gv)
    {
        return;
    }
    const char* raw = gv->mapname.ToCStr();
    if (!raw || !*raw)
    {
        return;
    }
    std::string realMap = NormalizeMapName(raw);
    if (realMap.empty())
    {
        return;
    }
    if (g_CurrentMap != realMap)
    {
        Dbg("EnsureCorrectMapLoaded: Map changed: '%s' -> '%s'", g_CurrentMap.c_str(), realMap.c_str());
        g_TempAccessSteamIDs.clear();
        LoadSettings();
        LoadPhrases();
        LoadDataForMap(realMap.c_str());
    }
    else
    {
        Dbg("EnsureCorrectMapLoaded: map='%s' (no change)", g_CurrentMap.c_str());
    }
}

static bool HasBpAccess(int slot)
{
    if (slot >= 0 && slot < 64 && g_pPlayers && g_pPlayers->IsInGame(slot))
    {
        uint64_t sid = g_pPlayers->GetSteamID64(slot);
        if (sid && g_TempAccessSteamIDs.count(sid))
        {
            return true;
        }
    }
    if (!g_pAdmin)
    {
        return false;
    }
    if (!g_AccessPermission.empty())
    {
        return g_pAdmin->HasPermission(slot, g_AccessPermission.c_str());
    }
    if (!g_AccessFlag.empty())
    {
        return g_pAdmin->HasFlag(slot, g_AccessFlag.c_str());
    }
    return false;
}

static bool OnBpCmd(int slot, const char*)
{
    if (!HasBpAccess(slot))
    {
        PrintChatKey(slot, "Chat_NoAccess", "{RED}Нет доступа к {DEFAULT}!bp{RED}.");
        return true;
    }

    OpenMainMenu(slot);
    return true;
}

static void OnMapStart(const char* map)
{
    g_TempAccessSteamIDs.clear();
    LoadSettings();
    LoadPhrases();

    const char* realMap = nullptr;
    CGlobalVars* gv = g_pUtils->GetCGlobalVars();
    if (gv)
    {
        realMap = gv->mapname.ToCStr();
    }
    if (!realMap || !*realMap)
    {
        realMap = map;
    }
    Dbg("OnMapStart: raw='%s' globals='%s'", map ? map : "(null)", realMap ? realMap : "(null)");
    LoadDataForMap(realMap);
}

static void OnMapEnd()
{
    ClearLive(true);
}

static inline void TeleportLive(int index);
static inline void MakeLiveIfMissing(int index);
static void RespawnWallLive(int index);

static void OnPlayerPingEvent(const char*, IGameEvent* pEvent, bool)
{
    if (!pEvent)
    {
        return;
    }
    int iSlot = pEvent->GetInt("userid");
    if (iSlot < 0 || iSlot >= 64)
    {
        return;
    }
    if (g_ePingMode[iSlot] == PING_NONE)
    {
        return;
    }

    Vector pingPos(pEvent->GetFloat("x"), pEvent->GetFloat("y"), pEvent->GetFloat("z"));

    if (g_ePingMode[iSlot] == PING_TELEPORT)
    {
        int iIndex = g_iPingTargetIndex[iSlot];
        if (iIndex < 0 || iIndex >= (int)g_Items.size())
        {
            g_ePingMode[iSlot] = PING_NONE;
            return;
        }

        if (g_Items[iIndex].isWall)
        {
            Vector center = (g_Items[iIndex].pos + g_Items[iIndex].pos2) * 0.5f;
            Vector offset = pingPos - center;
            g_Items[iIndex].pos += offset;
            g_Items[iIndex].pos2 += offset;
            RespawnWallLive(iIndex);
        }
        else
        {
            g_Items[iIndex].pos = pingPos;
            TeleportLive(iIndex);
            MakeLiveIfMissing(iIndex);
        }
        SaveData();
        g_ePingMode[iSlot] = PING_NONE;
        OpenItemMenu(iSlot, iIndex);
        return;
    }

    if (g_ePingMode[iSlot] == PING_WALL_POS1)
    {
        g_vWallTempPos[iSlot] = pingPos;
        g_ePingMode[iSlot] = PING_WALL_POS2;
        PrintChatKey(iSlot, "Chat_WallPos2", "Теперь поставьте вторую точку пингом (колёсико мышки)");
        return;
    }

    if (g_ePingMode[iSlot] == PING_WALL_POS2)
    {
        g_ePingMode[iSlot] = PING_NONE;

        BPItem it;
        it.label = "Стена";
        it.path = "";
        it.isWall = true;
        it.pos = g_vWallTempPos[iSlot];
        it.pos2 = pingPos;
        it.scale = 1.0f;
        it.invisible = false;

        int newIndex = (int)g_Items.size();
        g_Items.push_back(it);
        SaveData();

        LiveEnt le;
        le.index = newIndex;
        SpawnLiveEntry(newIndex, le);
        g_Live.push_back(std::move(le));

        PrintChatKey(iSlot, "Chat_WallCreated", "Стена создана!");
        OpenItemMenu(iSlot, newIndex);
        return;
    }
}

static void OnRoundStartEvent(const char*, IGameEvent*, bool)
{
    ClearLive(true);
    EnsureCorrectMapLoaded();
    for (int i = 0; i < 64; ++i)
    {
        g_ePingMode[i] = PING_NONE;
        g_iPingTargetIndex[i] = -1;
    }
    g_pUtils->CreateTimer(0.10f, []() -> float {
        ApplyState();
        return -1.0f;
    });
    if (!ShouldBeOpen())
    {
        PrintChatAllKey("Chat_ClosedMsg", "{RED}[BP]{DEFAULT} Проход закрыт. Откроется при {RED}%d{DEFAULT} игроках.", g_MinPlayersToOpen);
    }
}

void StartupServer()
{
    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    gpGlobals = g_pUtils->GetCGlobalVars();
}

static inline void TeleportLive(int index)
{
    if (g_Items[index].isWall)
    {
        return;
    }
    for (auto& le : g_Live)
    {
        if (le.index == index && le.ent.Get())
        {
            g_pUtils->TeleportEntity((CBaseEntity*)le.ent.Get(), &g_Items[index].pos, &g_Items[index].ang, nullptr);
            return;
        }
    }
}

static void RespawnWallLive(int index);

static inline void MakeLiveIfMissing(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index == index)
        {
            return;
        }
    }
    if (!ShouldBeOpen())
    {
        if (g_Items[index].isWall)
        {
            LiveEnt le;
            le.index = index;
            SpawnLiveEntry(index, le);
            g_Live.push_back(std::move(le));
        }
        else
        {
            CBaseEntity* e = SpawnOne(g_Items[index]);
            if (e)
            {
                LiveEnt le;
                le.index = index;
                le.ent = CHandle<CBaseEntity>(e);
                g_Live.push_back(std::move(le));
            }
            else
            {
                Dbg("MakeLiveIfMissing: failed for %d", index);
            }
        }
    }
}

static void RespawnLive(int index)
{
    bool isWall = (index >= 0 && index < (int)g_Items.size() && g_Items[index].isWall);
    for (auto it = g_Live.begin(); it != g_Live.end(); ++it)
    {
        if (it->index == index)
        {
            if (isWall)
            {
                for (auto& wc : it->wallColls)
                {
                    if (wc.Get())
                    {
                        KillWallCollision(wc.Get());
                    }
                }
                it->wallColls.clear();
            }
            else if (it->ent.Get())
            {
                g_pUtils->RemoveEntity((CEntityInstance*)it->ent.Get());
            }
            RemoveLiveBeams(*it);
            g_Live.erase(it);
            break;
        }
    }

    if (!ShouldBeOpen())
    {
        if (g_Items[index].isWall)
        {
            LiveEnt le;
            le.index = index;
            SpawnLiveEntry(index, le);
            g_Live.push_back(std::move(le));
        }
        else
        {
            CBaseEntity* e = SpawnOne(g_Items[index]);
            if (e)
            {
                LiveEnt le;
                le.index = index;
                le.ent = CHandle<CBaseEntity>(e);
                g_Live.push_back(std::move(le));
            }
            else
            {
                Dbg("RespawnLive: spawn failed for %d", index);
            }
        }
    }
}

static void RespawnWallLive(int index)
{
    RespawnLive(index);
}

static inline void ApplyVisualScaleToLive(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index != index || !le.ent.Get())
        {
            continue;
        }

        float s = ClampScale(g_Items[index].scale);

        auto* body = le.ent.Get()->m_CBodyComponent();
        if (body)
        {
            auto* node = body->m_pSceneNode();
            if (node)
            {
                node->m_flScale() = s;
                g_pUtils->SetStateChanged(le.ent.Get(), "CBaseEntity", "m_CBodyComponent");
                Dbg("ApplyVisualScaleToLive: idx=%d sceneNode scale=%.3f", index, s);
                return;
            }
        }

        RespawnLive(index);
        return;
    }
}

static inline void ApplyInvisibilityToLive(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index != index || !le.ent.Get())
        {
            continue;
        }
        bool inv = g_Items[index].invisible;
        auto* me = dynamic_cast<CBaseModelEntity*>(le.ent.Get());
        ApplyRenderAlpha(me, inv ? 0 : 255);
        SetNoDraw(le.ent.Get(), inv);
        if (!inv)
        {
            ApplyRenderColor(me, g_Items[index].itemR, g_Items[index].itemG, g_Items[index].itemB);
        }
        Dbg("ApplyInvisibilityToLive: idx=%d invisible=%d", index, (int)inv);
        return;
    }
}

static void OpenMainMenu(int slot)
{
    if (!g_pMenus)
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_Title", "BlockerPasses"));
    g_pMenus->AddItemMenu(m, "place", Phrase("Menu_Place", "Поставить предмет"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "wall", Phrase("Menu_Wall", "Создать стену (beam)"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "edit", Phrase("Menu_Edit", "Редактировать предметы"), ITEM_DEFAULT);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "place"))
        {
            OpenModelMenu(iSlot);
        }
        else if (!strcmp(back, "wall"))
        {
            g_ePingMode[iSlot] = PING_WALL_POS1;
            PrintChatKey(iSlot, "Chat_WallPos1", "Поставьте первую точку пингом (колёсико мышки)");
            g_pMenus->ClosePlayerMenu(iSlot);
        }
        else if (!strcmp(back, "edit"))
        {
            int idx = FindItemByCrosshair(iSlot, 128.0f);
            if (idx >= 0)
            {
                MakeLiveIfMissing(idx);
                OpenItemMenu(iSlot, idx);
            }
            else
            {
                OpenEditListMenu(iSlot);
            }
        }
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenModelMenu(int slot)
{
    if (!g_pMenus)
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_ModelTitle", "Выбор модели"));
    if (g_ModelDefs.empty())
    {
        g_pMenus->AddItemMenu(m, "none", Phrase("Menu_NoModels", "Нет моделей"), ITEM_DISABLED);
    }
    else
    {
        for (size_t i = 0; i < g_ModelDefs.size(); ++i)
        {
            char key[64];
            V_snprintf(key, sizeof(key), "m:%zu", i);
            g_pMenus->AddItemMenu(m, key, g_ModelDefs[i].label.c_str(), ITEM_DEFAULT);
        }
    }
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenMainMenu(iSlot);
            return;
        }

        if (strncmp(back, "m:", 2))
        {
            return;
        }
        int idx = atoi(back + 2);
        if (idx < 0 || idx >= (int)g_ModelDefs.size())
        {
            return;
        }

        trace_info_t tr = g_pPlayers->RayTrace(iSlot);
        Vector pos = tr.m_vEndPos;
        QAngle ang = {0.f, 0.f, 0.f};

        BPItem it;
        it.label = g_ModelDefs[idx].label;
        it.path = g_ModelDefs[idx].path;
        it.pos = pos;
        it.ang = ang;
        it.scale = 1.0f;
        it.invisible = false;

        int newIndex = (int)g_Items.size();
        g_Items.push_back(it);
        SaveData();

        MakeLiveIfMissing(newIndex);
        OpenItemMenu(iSlot, newIndex);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenEditListMenu(int slot)
{
    if (!g_pMenus)
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_EditPick", "Выбери предмет"));
    if (g_Items.empty())
    {
        g_pMenus->AddItemMenu(m, "none", Phrase("Menu_NoItems", "Нет предметов"), ITEM_DISABLED);
    }
    else
    {
        for (int i = 0; i < (int)g_Items.size(); ++i)
        {
            char key[64];
            V_snprintf(key, sizeof(key), "e:%d", i);
            std::string title = g_Items[i].label.empty() ? g_Items[i].path : g_Items[i].label;
            if (g_Items[i].isWall)
            {
                title += " [wall]";
            }
            g_pMenus->AddItemMenu(m, key, title.c_str(), ITEM_DEFAULT);
        }
    }
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenMainMenu(iSlot);
            return;
        }
        if (strncmp(back, "e:", 2))
        {
            return;
        }
        int idx = atoi(back + 2);
        if (idx < 0 || idx >= (int)g_Items.size())
        {
            return;
        }
        MakeLiveIfMissing(idx);
        OpenItemMenu(iSlot, idx);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenWallMoveMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_MoveTitle", "Движение"));
    g_pMenus->AddItemMenu(m, "x;10", "По оси X +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "x;-10", "По оси X -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;10", "По оси Y +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;-10", "По оси Y -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;10", "По оси Z +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;-10", "По оси Z -10", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }
        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        float dx = 0, dy = 0, dz = 0;
        if (!strcmp(back, "x;10")) dx = 10;
        else if (!strcmp(back, "x;-10")) dx = -10;
        else if (!strcmp(back, "y;10")) dy = 10;
        else if (!strcmp(back, "y;-10")) dy = -10;
        else if (!strcmp(back, "z;10")) dz = 10;
        else if (!strcmp(back, "z;-10")) dz = -10;
        else return;

        g_Items[index].pos.x += dx;
        g_Items[index].pos.y += dy;
        g_Items[index].pos.z += dz;
        g_Items[index].pos2.x += dx;
        g_Items[index].pos2.y += dy;
        g_Items[index].pos2.z += dz;

        RespawnWallLive(index);
        SaveData();
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenWallScaleMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();

    Vector diff = g_Items[index].pos2 - g_Items[index].pos;
    float sizeX = fabsf(diff.x);
    float sizeY = fabsf(diff.y);
    float sizeZ = fabsf(diff.z);

    char title[128];
    V_snprintf(title, sizeof(title), "%s {%.0fx%.0fx%.0f}", Phrase("Menu_WallScaleTitle", "Размер стены"), sizeX, sizeY, sizeZ);
    g_pMenus->SetTitleMenu(m, title);

    g_pMenus->AddItemMenu(m, "s;10", "Увеличить +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "s;-10", "Уменьшить -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "s;50", "Увеличить +50", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "s;-50", "Уменьшить -50", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "s;100", "Увеличить +100", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "s;-100", "Уменьшить -100", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }
        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        float delta = 0.0f;
        if (sscanf(back, "s;%f", &delta) != 1)
        {
            return;
        }

        Vector center = (g_Items[index].pos + g_Items[index].pos2) * 0.5f;
        Vector half = (g_Items[index].pos2 - g_Items[index].pos) * 0.5f;

        for (int a = 0; a < 3; ++a)
        {
            if (half[a] > 0)
            {
                half[a] = fmaxf(half[a] + delta * 0.5f, 1.0f);
            }
            else if (half[a] < 0)
            {
                half[a] = fminf(half[a] - delta * 0.5f, -1.0f);
            }
            else
            {
                half[a] = delta * 0.5f;
            }
        }

        g_Items[index].pos = center - half;
        g_Items[index].pos2 = center + half;

        RespawnWallLive(index);
        SaveData();
        OpenWallScaleMenu(iSlot, index);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenWallRotateMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();

    char title[128];
    V_snprintf(title, sizeof(title), "%s {%.0f°}", Phrase("Menu_WallRotateTitle", "Поворот стены"), g_Items[index].wallYaw);
    g_pMenus->SetTitleMenu(m, title);

    g_pMenus->AddItemMenu(m, "r;45", "+45°", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "r;-45", "-45°", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "r;15", "+15°", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "r;-15", "-15°", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "r;5", "+5°", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "r;-5", "-5°", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }
        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        float delta = 0.0f;
        if (sscanf(back, "r;%f", &delta) == 1)
        {
            g_Items[index].wallYaw += delta;
            if (g_Items[index].wallYaw >= 360.0f)
            {
                g_Items[index].wallYaw -= 360.0f;
            }
            if (g_Items[index].wallYaw < 0.0f)
            {
                g_Items[index].wallYaw += 360.0f;
            }
            RespawnWallLive(index);
            SaveData();
            OpenWallRotateMenu(iSlot, index);
        }
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenItemMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();

    bool isWall = g_Items[index].isWall;

    char title[256];
    V_snprintf(title, sizeof(title), "%s%s", g_Items[index].label.c_str(), isWall ? " [wall]" : "");
    g_pMenus->SetTitleMenu(m, title);

    if (isWall)
    {
        g_pMenus->AddItemMenu(m, "wallmove", Phrase("Menu_Move", "Двигать"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "wallscale", Phrase("Menu_WallScale", "Размер стены"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "wallrotate", Phrase("Menu_WallRotate", "Поворот стены"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "beamcolor", Phrase("Menu_BeamColor", "Цвет лазера"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "wall:trace", Phrase("Menu_MoveTrace", "Перенести в точку прицела"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "wallping", Phrase("Menu_PingMove", "Телепортировать пингом"), ITEM_DEFAULT);
    }

    if (!isWall)
    {
        g_pMenus->AddItemMenu(m, "move", Phrase("Menu_Move", "Двигать"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "rotate", Phrase("Menu_Rotate", "Поворачивать"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "scale", Phrase("Menu_Scale", "Масштаб"), ITEM_DEFAULT);
    }

    g_pMenus->AddItemMenu(m, "teleport", Phrase("Menu_Teleport", "Телепортироваться"), ITEM_DEFAULT);

    if (!isWall)
    {
        g_pMenus->AddItemMenu(m, "itemcolor", Phrase("Menu_ItemColor", "Цвет предмета"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "ping", Phrase("Menu_PingMove", "Телепортировать пингом"), ITEM_DEFAULT);
        g_pMenus->AddItemMenu(m, "move:trace", Phrase("Menu_MoveTrace", "Перенести в точку прицела"), ITEM_DEFAULT);

        if (g_Items[index].invisible)
        {
            g_pMenus->AddItemMenu(m, "invis:off", Phrase("Menu_InvisOff", "Показать"), ITEM_DEFAULT);
        }
        else
        {
            g_pMenus->AddItemMenu(m, "invis:on", Phrase("Menu_InvisOn", "Скрыть"), ITEM_DEFAULT);
        }
    }

    g_pMenus->AddItemMenu(m, "delete", Phrase("Menu_Delete", "Удалить"), ITEM_DEFAULT);

    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);

    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenEditListMenu(iSlot);
            return;
        }

        if (!strcmp(back, "move"))
        {
            OpenMoveMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "rotate"))
        {
            OpenRotateSubMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "scale"))
        {
            OpenScaleMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "wallmove"))
        {
            OpenWallMoveMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "wallscale"))
        {
            OpenWallScaleMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "beamcolor"))
        {
            OpenBeamColorMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "wallrotate"))
        {
            OpenWallRotateMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "wall:trace"))
        {
            trace_info_t tr = g_pPlayers->RayTrace(iSlot);
            Vector center = (g_Items[index].pos + g_Items[index].pos2) * 0.5f;
            Vector offset = tr.m_vEndPos - center;
            g_Items[index].pos += offset;
            g_Items[index].pos2 += offset;
            RespawnWallLive(index);
            SaveData();
            OpenItemMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "wallping"))
        {
            g_ePingMode[iSlot] = PING_TELEPORT;
            g_iPingTargetIndex[iSlot] = index;
            PrintChatKey(iSlot, "Chat_UsePing", "Выберите место с помощью пинга (колёсико мышки)");
            g_pMenus->ClosePlayerMenu(iSlot);
            return;
        }
        if (!strcmp(back, "itemcolor"))
        {
            OpenItemColorMenu(iSlot, index);
            return;
        }

        if (!strcmp(back, "teleport"))
        {
            CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(iSlot);
            if (!pPlayer || !pPlayer->GetPlayerPawn() || !pPlayer->GetPlayerPawn()->IsAlive())
            {
                PrintChatKey(iSlot, "Chat_MustBeAlive", "Для этого вы должны быть живы");
                return;
            }
            g_pUtils->TeleportEntity(pPlayer->GetPlayerPawn(), &g_Items[index].pos, &g_Items[index].ang, nullptr);
            OpenItemMenu(iSlot, index);
            return;
        }

        if (!strcmp(back, "ping"))
        {
            g_ePingMode[iSlot] = PING_TELEPORT;
            g_iPingTargetIndex[iSlot] = index;
            PrintChatKey(iSlot, "Chat_UsePing", "Выберите место с помощью пинга (колёсико мышки)");
            g_pMenus->ClosePlayerMenu(iSlot);
            return;
        }

        if (!strcmp(back, "move:trace"))
        {
            trace_info_t tr = g_pPlayers->RayTrace(iSlot);
            g_Items[index].pos = tr.m_vEndPos;
            TeleportLive(index);
            MakeLiveIfMissing(index);
            SaveData();
            OpenItemMenu(iSlot, index);
            return;
        }

        if (!strcmp(back, "invis:on"))
        {
            g_Items[index].invisible = true;
            ApplyInvisibilityToLive(index);
            SaveData();
            OpenItemMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "invis:off"))
        {
            g_Items[index].invisible = false;
            ApplyInvisibilityToLive(index);
            SaveData();
            OpenItemMenu(iSlot, index);
            return;
        }

        if (!strcmp(back, "delete"))
        {
            bool isWallDel = (index >= 0 && index < (int)g_Items.size() && g_Items[index].isWall);
            for (auto it = g_Live.begin(); it != g_Live.end(); )
            {
                if (it->index == index)
                {
                    if (isWallDel)
                    {
                        for (auto& wc : it->wallColls)
                        {
                            if (wc.Get())
                            {
                                KillWallCollision(wc.Get());
                            }
                        }
                        it->wallColls.clear();
                    }
                    else if (it->ent.Get())
                    {
                        g_pUtils->RemoveEntity((CEntityInstance*)it->ent.Get());
                    }
                    RemoveLiveBeams(*it);
                    it = g_Live.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            g_Items.erase(g_Items.begin() + index);
            for (auto& le : g_Live)
            {
                if (le.index > index)
                {
                    le.index--;
                }
            }
            SaveData();
            OpenEditListMenu(iSlot);
            return;
        }
    });

    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenMoveMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_MoveTitle", "Движение"));
    g_pMenus->AddItemMenu(m, "x;10", "По оси X +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "x;-10", "По оси X -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;10", "По оси Y +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;-10", "По оси Y -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;10", "По оси Z +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;-10", "По оси Z -10", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }

        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        Vector& pos = g_Items[index].pos;
        if (!strcmp(back, "x;10"))
        {
            pos.x += 10;
        }
        else if (!strcmp(back, "x;-10"))
        {
            pos.x -= 10;
        }
        else if (!strcmp(back, "y;10"))
        {
            pos.y += 10;
        }
        else if (!strcmp(back, "y;-10"))
        {
            pos.y -= 10;
        }
        else if (!strcmp(back, "z;10"))
        {
            pos.z += 10;
        }
        else if (!strcmp(back, "z;-10"))
        {
            pos.z -= 10;
        }
        else
        {
            return;
        }

        TeleportLive(index);
        MakeLiveIfMissing(index);
        SaveData();
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenRotateSubMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_RotateTitle", "Поворот"));
    g_pMenus->AddItemMenu(m, "x;10", "По оси X +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "x;-10", "По оси X -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;10", "По оси Y +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "y;-10", "По оси Y -10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;10", "По оси Z +10", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "z;-10", "По оси Z -10", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }

        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        QAngle& ang = g_Items[index].ang;
        if (!strcmp(back, "x;10"))
        {
            ang.x += 10;
        }
        else if (!strcmp(back, "x;-10"))
        {
            ang.x -= 10;
        }
        else if (!strcmp(back, "y;10"))
        {
            ang.y += 10;
        }
        else if (!strcmp(back, "y;-10"))
        {
            ang.y -= 10;
        }
        else if (!strcmp(back, "z;10"))
        {
            ang.z += 10;
        }
        else if (!strcmp(back, "z;-10"))
        {
            ang.z -= 10;
        }
        else
        {
            return;
        }

        TeleportLive(index);
        MakeLiveIfMissing(index);
        SaveData();
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenScaleMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();

    char title[128];
    V_snprintf(title, sizeof(title), "%s {%.2fx}", Phrase("Menu_ScaleTitle", "Масштаб"), g_Items[index].scale);
    g_pMenus->SetTitleMenu(m, title);

    g_pMenus->AddItemMenu(m, "0.1", "Увеличить +0.1", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "-0.1", "Уменьшить -0.1", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "0.5", "Увеличить +0.5", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "-0.5", "Уменьшить -0.5", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "1.0", "Увеличить +1.0", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "-1.0", "Уменьшить -1.0", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }

        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        float fDelta = (float)atof(back);
        g_Items[index].scale = ClampScale(g_Items[index].scale + fDelta);
        ApplyVisualScaleToLive(index);
        SaveData();
        OpenScaleMenu(iSlot, index);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void RespawnWallBeams(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index != index)
        {
            continue;
        }
        RemoveLiveBeams(le);
        const BPItem& it = g_Items[index];
        le.beams = DrawWireframe(it.pos, it.pos2, it.beamR, it.beamG, it.beamB, it.beamRainbow, it.wallYaw);
        if (it.beamRainbow)
        {
            StartRainbowTimer();
        }
        return;
    }
}

static inline void ApplyItemColorToLive(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index != index || !le.ent.Get())
        {
            continue;
        }
        auto* me = dynamic_cast<CBaseModelEntity*>(le.ent.Get());
        ApplyRenderColor(me, g_Items[index].itemR, g_Items[index].itemG, g_Items[index].itemB);
        return;
    }
}

static void OpenItemColorMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_ItemColorTitle", "Цвет предмета"));
    g_pMenus->AddItemMenu(m, "c:255:255:255", "Белый (по умолчанию)", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:0:0", "Красный", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:0:255:0", "Зелёный", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:0:128:255", "Голубой", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:255:0", "Жёлтый", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:0:255", "Розовый", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:0:0:0", "Чёрный", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }
        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }
        if (!strncmp(back, "c:", 2))
        {
            int r = 255, g = 255, b = 255;
            sscanf(back, "c:%d:%d:%d", &r, &g, &b);
            g_Items[index].itemR = r;
            g_Items[index].itemG = g;
            g_Items[index].itemB = b;
            ApplyItemColorToLive(index);
            SaveData();
        }
        OpenItemMenu(iSlot, index);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenBeamColorMenu(int slot, int index)
{
    if (!g_pMenus || index < 0 || index >= (int)g_Items.size())
    {
        return;
    }
    Menu m;
    m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_BeamColorTitle", "Цвет лазера"));
    g_pMenus->AddItemMenu(m, "c:255:0:0", "Красный", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:0:255:0", "Зелёный", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:0:128:255", "Голубой", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:255:0", "Жёлтый", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:0:255", "Розовый", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "c:255:255:255", "Белый", ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "rainbow", "Разноцветный", ITEM_DEFAULT);
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot) {
        if (!strcmp(back, "back"))
        {
            OpenItemMenu(iSlot, index);
            return;
        }
        if (index < 0 || index >= (int)g_Items.size())
        {
            return;
        }

        if (!strcmp(back, "rainbow"))
        {
            g_Items[index].beamRainbow = true;
        }
        else if (!strncmp(back, "c:", 2))
        {
            int r = 0, g = 128, b = 255;
            sscanf(back, "c:%d:%d:%d", &r, &g, &b);
            g_Items[index].beamR = r;
            g_Items[index].beamG = g;
            g_Items[index].beamB = b;
            g_Items[index].beamRainbow = false;
        }
        else
        {
            return;
        }

        RespawnWallBeams(index);
        SaveData();
        OpenItemMenu(iSlot, index);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils->GetCGameEntitySystem();
}

bool BlockerPasses::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    if (!g_pSource2Server)
    {
        GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    }

    g_SMAPI->AddListener(this, this);
    return true;
}

bool BlockerPasses::Unload(char* error, size_t maxlen)
{
    ConVar_Unregister();
    if (g_pUtils)
    {
        g_pUtils->ClearAllHooks(g_PLID);
    }
    ClearLive(true);
    return true;
}

void BlockerPasses::AllPluginsLoaded()
{
    char error[64];
    int ret;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pPlayers)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());    
        return;
    }

    g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Menus system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pAdmin = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pAdmin)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Admin system plugin");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
    }

    LoadSettings();
    LoadPhrases();

    if (g_pSource2Server)
    {
        DynLibUtils::CModule libserver(g_pSource2Server);
        auto fnAddr = libserver.FindPattern("48 81 C7 ? ? ? ? E9 ? ? ? ? CC CC CC CC 55 48 8D 15");
        if (fnAddr)
        {
            g_fnSetCollisionBounds = fnAddr.RCast<SetCollisionBounds_t>();
            ConColorMsg(Color(0, 255, 0, 255), "[BlockerPasses] SetCollisionBounds found at %p\n", g_fnSetCollisionBounds);
        }
        else
        {
            ConColorMsg(Color(255, 0, 0, 255), "[BlockerPasses] Failed to find SetCollisionBounds signature! Walls won't block.\n");
        }
    }

    g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pUtils->MapStartHook(g_PLID, OnMapStart);
    g_pUtils->MapEndHook(g_PLID, OnMapEnd);
    g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStartEvent);
    g_pUtils->HookEvent(g_PLID, "player_ping", OnPlayerPingEvent);

    g_pUtils->RegCommand(g_PLID, {"mm_bp"}, {"!bp"}, OnBpCmd);

    g_pUtils->RegCommand(g_PLID, {"mm_bp_access"}, {}, [](int slot, const char* args) -> bool {
        if (slot >= 0)
        {
            return true;
        }
        if (!args || !*args)
        {
            ConColorMsg(Color(255, 255, 0, 255), "[BlockerPasses] Usage: mm_bp_access <steamid64>\n");
            return true;
        }
        char buf[128];
        V_strncpy(buf, args, sizeof(buf));
        char* tok = strtok(buf, " ");
        if (tok)
        {
            tok = strtok(nullptr, " ");
        }
        if (!tok || !*tok)
        {
            ConColorMsg(Color(255, 255, 0, 255), "[BlockerPasses] Usage: mm_bp_access <steamid64>\n");
            return true;
        }
        uint64_t sid = strtoull(tok, nullptr, 10);
        if (sid == 0)
        {
            ConColorMsg(Color(255, 0, 0, 255), "[BlockerPasses] Invalid SteamID64\n");
            return true;
        }
        g_TempAccessSteamIDs.insert(sid);
        ConColorMsg(Color(0, 255, 0, 255), "[BlockerPasses] Access granted to %llu (until map change)\n", (unsigned long long)sid);
        return true;
    });

    g_pUtils->RegCommand(g_PLID, {"mm_bp_walls"}, {}, [](int slot, const char* args) -> bool {
        if (slot >= 0)
        {
            return true;
        }
        ConColorMsg(Color(150, 200, 255, 255), "[BlockerPasses] === Wall Debug Info ===\n");
        ConColorMsg(Color(150, 200, 255, 255), "[BlockerPasses] Items: %d, Live: %d\n", (int)g_Items.size(), (int)g_Live.size());

        int wallCount = 0;
        for (int i = 0; i < (int)g_Items.size(); ++i)
        {
            if (!g_Items[i].isWall)
            {
                continue;
            }
            wallCount++;
            const BPItem& it = g_Items[i];
            ConColorMsg(Color(200, 200, 200, 255),
                "[BlockerPasses] Wall item[%d] \"%s\" p1(%.1f %.1f %.1f) p2(%.1f %.1f %.1f) yaw=%.1f\n",
                i, it.label.c_str(), it.pos.x, it.pos.y, it.pos.z, it.pos2.x, it.pos2.y, it.pos2.z, it.wallYaw);
        }

        int liveWalls = 0;
        for (auto& le : g_Live)
        {
            if (le.index < 0 || le.index >= (int)g_Items.size() || !g_Items[le.index].isWall)
            {
                continue;
            }
            liveWalls++;
            CBaseEntity* ent = le.ent.Get();
            if (ent)
            {
                auto* me = dynamic_cast<CBaseModelEntity*>(ent);
                if (me)
                {
                    Vector entPos = ent->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin();
                    Vector mins = me->m_Collision().m_vecMins();
                    Vector maxs = me->m_Collision().m_vecMaxs();
                    int solid = me->m_Collision().m_nSolidType();
                    ConColorMsg(Color(0, 255, 0, 255),
                        "[BlockerPasses]   Live[idx=%d] VALID ent pos(%.1f %.1f %.1f) solid=%d mins(%.1f %.1f %.1f) maxs(%.1f %.1f %.1f) beams=%d\n",
                        le.index, entPos.x, entPos.y, entPos.z, solid,
                        mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z, (int)le.beams.size());
                    ConColorMsg(Color(0, 255, 0, 255),
                        "[BlockerPasses]     wallColls=%d\n", (int)le.wallColls.size());
                }
                else
                {
                    ConColorMsg(Color(255, 255, 0, 255),
                        "[BlockerPasses]   Live[idx=%d] ent exists but not CBaseModelEntity, beams=%d\n",
                        le.index, (int)le.beams.size());
                }
            }
            else
            {
                ConColorMsg(Color(255, 0, 0, 255),
                    "[BlockerPasses]   Live[idx=%d] NULL entity handle! beams=%d\n",
                    le.index, (int)le.beams.size());
            }
        }
        ConColorMsg(Color(150, 200, 255, 255), "[BlockerPasses] Total: %d wall items, %d live walls\n", wallCount, liveWalls);
        return true;
    });
}

const char* BlockerPasses::GetLicense()
{
    return "GPL";
}

const char* BlockerPasses::GetVersion()
{
    return "2.0";
}

const char* BlockerPasses::GetDate()
{
    return __DATE__;
}

const char* BlockerPasses::GetLogTag()
{
    return "[BlockerPasses]";
}

const char* BlockerPasses::GetAuthor()
{
    return "ABKAM";
}

const char* BlockerPasses::GetDescription()
{
    return "BlockerPasses";
}

const char* BlockerPasses::GetName()
{
    return "BlockerPasses";
}

const char* BlockerPasses::GetURL()
{
    return "https://discord.gg/ChYfTtrtmS";
}
