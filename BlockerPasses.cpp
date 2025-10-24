#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include "BlockerPasses.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

BlockerPasses g_BlockerPasses;
PLUGIN_EXPOSE(BlockerPasses, g_BlockerPasses);

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IMenusApi* g_pMenus = nullptr;
IAdminApi* g_pAdmin = nullptr;

struct ModelDef { std::string label; std::string path; };
struct BPItem { std::string label; std::string path; Vector pos; QAngle ang; float scale = 1.0f; bool invisible = false; };
struct LiveEnt { int index; CBaseEntity* ent; };

static std::vector<ModelDef> g_ModelDefs;
static std::vector<BPItem>   g_Items;
static std::vector<LiveEnt>  g_Live;

static std::string g_CurrentMap;

static int g_MinPlayersToOpen = 10;
static bool g_DebugLog = true;
static std::string g_AccessPermission = "@admin/bp"; 
static std::string g_AccessFlag = "";

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
        return;

    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues *p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
        g_Phrases[p->GetName()] = p->GetString(lang);
}

static inline void PrintChatRaw(int slot, const char* fmt, va_list va)
{
    if (!g_pUtils) return;
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
    va_list va; va_start(va, fmt);
    PrintChatRaw(slot, fmt, va);
    va_end(va);
}

static inline void PrintChatKey(int slot, const char* key, const char* fallbackFmt, ...)
{
    const char* fmt = Phrase(key, fallbackFmt);
    va_list va; va_start(va, fallbackFmt);
    PrintChatRaw(slot, fmt, va);
    va_end(va);
}

static inline void PrintChatAllKey(const char* key, const char* fallbackFmt, ...)
{
    if (!g_pUtils) return;
    const char* fmt = Phrase(key, fallbackFmt);
    char msg[1024];
    va_list va; va_start(va, fallbackFmt);
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
    if (!in || !*in) return "";
    std::string s(in);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    size_t pos = s.find_last_of('/');
    if (pos != std::string::npos) s = s.substr(pos + 1);
    pos = s.find_last_of('.');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

static inline int HumansOnline()
{
    int c = 0;
    for (int i=0;i<64;++i)
        if (g_pPlayers->IsInGame(i) && !g_pPlayers->IsFakeClient(i)) ++c;
    return c;
}
static inline bool ShouldBeOpen() 
{ 
	return HumansOnline() >= g_MinPlayersToOpen; 
}

static void Dbg(const char* fmt, ...)
{
    if (!g_DebugLog) return;
    char buf[1024];
    va_list va; va_start(va, fmt);
    V_vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    ConColorMsg(Color(150, 200, 255, 255), "[BlockerPasses] %s\n", buf);
}

static void ClearLive(bool removeEntities)
{
    if (removeEntities)
        for (auto& le : g_Live) if (le.ent) g_pUtils->RemoveEntity((CEntityInstance*)le.ent);
    g_Live.clear();
}

static inline float ClampScale(float v)
{
    if (v < 0.05f) v = 0.05f;
    if (v > 20.0f) v = 20.0f;
    return v;
}

static inline void ApplyRenderAlpha(CBaseModelEntity* ent, uint8_t a)
{
    if (!ent) return;
    if (ent->m_clrRender().a() == a) return;
    ent->m_clrRender() = Color(255,255,255,a);
    g_pUtils->SetStateChanged(ent, "CBaseModelEntity", "m_clrRender");
}

static inline void SetNoDraw(CBaseEntity* ent, bool on)
{
    if (!ent) return;
    int fx = ent->m_fEffects();
    int nf = on ? (fx | EF_NODRAW) : (fx & ~EF_NODRAW);
    if (nf == fx) return;
    ent->m_fEffects() = nf;
    g_pUtils->SetStateChanged(ent, "CBaseEntity", "m_fEffects");
}

static CBaseEntity* SpawnOne(const BPItem& it)
{
    if (it.path.empty()) { Dbg("SpawnOne: empty path"); return nullptr; }
    if (!strstr(it.path.c_str(), ".vmdl")) { Dbg("SpawnOne: invalid model '%s'", it.path.c_str()); return nullptr; }

    const char* classes[] = { "prop_dynamic", "prop_dynamic_override" };
    for (const char* cls : classes)
    {
        CBaseEntity* ent = (CBaseEntity*)g_pUtils->CreateEntityByName(cls, CEntityIndex(-1));
        if (!ent) { Dbg("SpawnOne: CreateEntityByName failed for %s", cls); continue; }

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
    for (auto& le : g_Live) if (le.ent) aliveIdx.push_back(le.index);

    for (int i = 0; i < (int)g_Items.size(); ++i)
    {
        if (std::find(aliveIdx.begin(), aliveIdx.end(), i) != aliveIdx.end()) continue;
        if (CBaseEntity* e = SpawnOne(g_Items[i])) g_Live.push_back({i, e});
        else Dbg("EnsureClosed: spawn failed for %d", i);
    }
}

static void EnsureOpen()
{
    ClearLive(true);
}

static void ApplyState()
{
    if (ShouldBeOpen()) EnsureOpen(); else EnsureClosed();
}

static void SaveData()
{
    const char* path = "addons/data/bp_data.ini";

    KeyValues::AutoDelete root("BPData");
    root->LoadFromFile(g_pFullFileSystem, path);

    if (KeyValues* old = root->FindKey(g_CurrentMap.c_str(), false))
        root->RemoveSubKey(old);

    KeyValues* mapKV = root->FindKey(g_CurrentMap.c_str(), true);
    for (int i=0;i<(int)g_Items.size();++i)
    {
        const BPItem& it = g_Items[i];
        KeyValues* k = mapKV->CreateNewKey();
        k->SetName("item");
        k->SetString("label", it.label.c_str());
        k->SetString("path",  it.path.c_str());
        k->SetFloat("px", it.pos.x); k->SetFloat("py", it.pos.y); k->SetFloat("pz", it.pos.z);
        k->SetFloat("ax", it.ang.x); k->SetFloat("ay", it.ang.y); k->SetFloat("az", it.ang.z);
        k->SetFloat("sc", it.scale);
        k->SetInt("iv",  it.invisible ? 1 : 0);
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
    else
    {
        char tmp[256] = {0};
        if (g_pUtils && g_pUtils->GetCGlobalVars())
            g_SMAPI->Format(tmp, sizeof(tmp), "%s", g_pUtils->GetCGlobalVars()->mapname);
        g_CurrentMap = NormalizeMapName(tmp);
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
        it.path  = k->GetString("path",  "");
        it.pos.x = k->GetFloat("px", 0.f);
        it.pos.y = k->GetFloat("py", 0.f);
        it.pos.z = k->GetFloat("pz", 0.f);
        it.ang.x = k->GetFloat("ax", 0.f);
        it.ang.y = k->GetFloat("ay", 0.f);
        it.ang.z = k->GetFloat("az", 0.f);
        it.scale = k->GetFloat("sc", 1.0f);
        it.invisible = k->GetInt("iv", 0) != 0;
        if (!it.path.empty()) g_Items.push_back(std::move(it));
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

    g_ModelDefs.clear();
    if (KeyValues* models = kv->FindKey("models", false))
    {
        for (KeyValues* m = models->GetFirstTrueSubKey(); m; m = m->GetNextTrueSubKey())
        {
            const char* label = m->GetString("label", "");
            const char* path  = m->GetString("path",  "");
            if (path && *path)
            {
                ModelDef md;
                md.path  = path;
                md.label = (label && *label) ? label : path;
                g_ModelDefs.push_back(std::move(md));
            }
        }
    }

    if (g_ModelDefs.empty())
        Dbg("No models in settings.ini -> nothing to place");

    Dbg("Settings: min_players_to_open=%d, debug=%d, perm='%s', flag='%s', models=%d",
        g_MinPlayersToOpen, (int)g_DebugLog, g_AccessPermission.c_str(), g_AccessFlag.c_str(), (int)g_ModelDefs.size());
}

static void OpenModelMenu(int slot);
static void OpenMainMenu(int slot);
static void OpenEditListMenu(int slot);
static void OpenRotateMenu(int slot, int index);

static int FindItemByCrosshair(int slot, float maxDist = 128.0f)
{
    trace_info_t tr = g_pPlayers->RayTrace(slot);
    Vector hit = tr.m_vEndPos;

    int best = -1;
    float bestSq = maxDist * maxDist;

    for (auto& le : g_Live)
    {
        int idx = le.index;
        if (idx < 0 || idx >= (int)g_Items.size()) continue;
        Vector d = g_Items[idx].pos - hit;
        float dsq = d.x*d.x + d.y*d.y + d.z*d.z;
        if (dsq <= bestSq) { bestSq = dsq; best = idx; }
    }
    return best;
}

static void EnsureCorrectMapLoaded()
{
    char cur[256] = {0};
    if (g_pUtils && g_pUtils->GetCGlobalVars())
        g_SMAPI->Format(cur, sizeof(cur), "%s", g_pUtils->GetCGlobalVars()->mapname);

    std::string now = NormalizeMapName(cur);
    if (now != g_CurrentMap)
    {
        Dbg("Map mismatch: had '%s', now '%s' -> reload data", g_CurrentMap.c_str(), now.c_str());
        LoadDataForMap(now.c_str());
    }
}

static bool HasBpAccess(int slot)
{
    if (!g_pAdmin) return false;
    if (!g_AccessPermission.empty())
        return g_pAdmin->HasPermission(slot, g_AccessPermission.c_str());
    if (!g_AccessFlag.empty())
        return g_pAdmin->HasFlag(slot, g_AccessFlag.c_str());
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
    LoadSettings();
    LoadPhrases();
    LoadDataForMap(map);
}

static void OnMapEnd()
{
    ClearLive(true);
}

static void OnRoundStartEvent(const char*, IGameEvent*, bool)
{
    ClearLive(false);
    EnsureCorrectMapLoaded();
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
    for (auto& le : g_Live)
        if (le.index == index && le.ent) { g_pUtils->TeleportEntity((CBaseEntity*)le.ent, &g_Items[index].pos, &g_Items[index].ang, nullptr); return; }
}

static inline void MakeLiveIfMissing(int index)
{
    for (auto& le : g_Live) if (le.index == index) return;
    if (!ShouldBeOpen())
    {
        if (CBaseEntity* e = SpawnOne(g_Items[index])) g_Live.push_back({index, e});
        else Dbg("MakeLiveIfMissing: failed for %d", index);
    }
}

static void RespawnLive(int index)
{
    for (auto it = g_Live.begin(); it != g_Live.end(); ++it)
    {
        if (it->index == index)
        {
            if (it->ent) g_pUtils->RemoveEntity((CEntityInstance*)it->ent);
            g_Live.erase(it);
            break;
        }
    }

    if (!ShouldBeOpen())
    {
        if (CBaseEntity* e = SpawnOne(g_Items[index])) g_Live.push_back({index, e});
        else Dbg("RespawnLive: spawn failed for %d", index);
    }
}

static inline void ApplyVisualScaleToLive(int index)
{
    for (auto& le : g_Live)
    {
        if (le.index != index || !le.ent) continue;

        float s = ClampScale(g_Items[index].scale);

        auto* body = le.ent->m_CBodyComponent();
        if (body)
        {
            auto* node = body->m_pSceneNode();
            if (node)
            {
                node->m_flScale() = s;
                g_pUtils->SetStateChanged(le.ent, "CBaseEntity", "m_CBodyComponent");
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
        if (le.index != index || !le.ent) continue;
        bool inv = g_Items[index].invisible;
        auto* me = dynamic_cast<CBaseModelEntity*>(le.ent);
        ApplyRenderAlpha(me, inv ? 0 : 255);
        SetNoDraw(le.ent, inv);
        Dbg("ApplyInvisibilityToLive: idx=%d invisible=%d", index, (int)inv);
        return;
    }
}

static void OpenMainMenu(int slot)
{
    if (!g_pMenus) return;
    Menu m; m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_Title", "BlockerPasses"));
    g_pMenus->AddItemMenu(m, "place",  Phrase("Menu_Place", "Поставить предмет"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "edit",   Phrase("Menu_Edit",  "Редактировать предметы"), ITEM_DEFAULT);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot){
        if (!strcmp(back,"place")) OpenModelMenu(iSlot);
        else if (!strcmp(back,"edit"))
        {
            int idx = FindItemByCrosshair(iSlot, 128.0f);
            if (idx >= 0) { MakeLiveIfMissing(idx); OpenRotateMenu(iSlot, idx); }
            else { OpenEditListMenu(iSlot); }
        }
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenModelMenu(int slot)
{
    if (!g_pMenus) return;
    Menu m; m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_ModelTitle", "Выбор модели"));
    if (g_ModelDefs.empty())
        g_pMenus->AddItemMenu(m, "none", Phrase("Menu_NoModels", "Нет моделей"), ITEM_DISABLED);
    else
        for (size_t i=0;i<g_ModelDefs.size();++i)
        {
            char key[64]; V_snprintf(key,sizeof(key),"m:%zu",i);
            g_pMenus->AddItemMenu(m, key, g_ModelDefs[i].label.c_str(), ITEM_DEFAULT);
        }
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot){
        if (!strcmp(back,"back")) { OpenMainMenu(iSlot); return; }

        if (strncmp(back,"m:",2)) return;
        int idx = atoi(back+2);
        if (idx < 0 || idx >= (int)g_ModelDefs.size()) return;

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
        OpenMainMenu(iSlot);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenEditListMenu(int slot)
{
    if (!g_pMenus) return;
    Menu m; m.clear();
    g_pMenus->SetTitleMenu(m, Phrase("Menu_EditPick", "Выбери предмет"));
    if (g_Items.empty())
        g_pMenus->AddItemMenu(m, "none", Phrase("Menu_NoItems", "Нет предметов"), ITEM_DISABLED);
    else
        for (int i=0;i<(int)g_Items.size();++i)
        {
            char key[64]; V_snprintf(key,sizeof(key),"e:%d",i);
            std::string title = g_Items[i].label.empty() ? g_Items[i].path : g_Items[i].label;
            g_pMenus->AddItemMenu(m, key, title.c_str(), ITEM_DEFAULT);
        }
    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int iSlot){
        if (!strcmp(back,"back")) { OpenMainMenu(iSlot); return; }
        if (strncmp(back,"e:",2)) return;
        int idx = atoi(back+2);
        if (idx < 0 || idx >= (int)g_Items.size()) return;
        MakeLiveIfMissing(idx);
        OpenRotateMenu(iSlot, idx);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void OpenRotateMenu(int slot, int index)
{
    if (!g_pMenus) return;
    Menu m; m.clear();

    char title[256];
    const char* titleFmt = Phrase("Menu_EditTitle", "Редактирование: %s");
    V_snprintf(title, sizeof(title), titleFmt, g_Items[index].label.c_str());

    char withScale[320];
    V_snprintf(withScale, sizeof(withScale), "%s  {%.2fx}", title, g_Items[index].scale);
    g_pMenus->SetTitleMenu(m, withScale);

    g_pMenus->AddItemMenu(m, "yaw:+15",  Phrase("Menu_YawP15",  "+ Yaw 15°"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "yaw:+5",   Phrase("Menu_YawP5",   "+ Yaw 5°"),  ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "yaw:+1",   Phrase("Menu_YawP1",   "+ Yaw 1°"),  ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "yaw:-1",   Phrase("Menu_YawM1",   "- Yaw 1°"),  ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "yaw:-5",   Phrase("Menu_YawM5",   "- Yaw 5°"),  ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "yaw:-15",  Phrase("Menu_YawM15",  "- Yaw 15°"), ITEM_DEFAULT);

    g_pMenus->AddItemMenu(m, "pitch:+5", Phrase("Menu_PitchP5", "+ Pitch 5°"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "pitch:-5", Phrase("Menu_PitchM5", "- Pitch 5°"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "roll:+5",  Phrase("Menu_RollP5",  "+ Roll 5°"),  ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "roll:-5",  Phrase("Menu_RollM5",  "- Roll 5°"),  ITEM_DEFAULT);

    g_pMenus->AddItemMenu(m, "scale:+0.10", Phrase("Menu_ScaleP01", "+ Size 0.10x"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "scale:-0.10", Phrase("Menu_ScaleM01", "- Size 0.10x"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "scale:+0.50", Phrase("Menu_ScaleP05", "+ Size 0.50x"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "scale:-0.50", Phrase("Menu_ScaleM05", "- Size 0.50x"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "scale:reset", Phrase("Menu_ScaleReset", "Reset size (1.0x)"), ITEM_DEFAULT);

    if (g_Items[index].invisible)
        g_pMenus->AddItemMenu(m, "invis:off", Phrase("Menu_InvisOff", "Show"), ITEM_DEFAULT);
    else
        g_pMenus->AddItemMenu(m, "invis:on",  Phrase("Menu_InvisOn",  "Hide"), ITEM_DEFAULT);

    g_pMenus->AddItemMenu(m, "move:trace", Phrase("Menu_MoveTrace", "Перенести в точку прицела"), ITEM_DEFAULT);

    g_pMenus->AddItemMenu(m, "save",     Phrase("Menu_Save",   "Сохранить"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "delete",   Phrase("Menu_Delete", "Удалить"),   ITEM_DEFAULT);

    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);

    g_pMenus->SetCallback(m, [index](const char* back, const char*, int, int iSlot){
        if (!strcmp(back,"back")) { OpenEditListMenu(iSlot); return; }

        if (!strcmp(back,"save"))
        {
            SaveData();
            OpenEditListMenu(iSlot);
            return;
        }
        if (!strcmp(back,"delete"))
        {
            for (auto it = g_Live.begin(); it != g_Live.end(); )
            {
                if (it->index == index)
                {
                    if (it->ent) g_pUtils->RemoveEntity((CEntityInstance*)it->ent);
                    it = g_Live.erase(it);
                }
                else ++it;
            }
            g_Items.erase(g_Items.begin()+index);
            for (auto& le : g_Live)
                if (le.index > index) le.index--;
            SaveData();
            OpenEditListMenu(iSlot);
            return;
        }
        if (!strcmp(back,"move:trace"))
        {
            trace_info_t tr = g_pPlayers->RayTrace(iSlot);
            g_Items[index].pos = tr.m_vEndPos;
            TeleportLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }

        if (!strncmp(back,"yaw:",4))
        {
            int sign = (back[4]=='+')?+1:-1;
            int val = atoi(back+5);
            g_Items[index].ang.y += sign * val;
            TeleportLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }
        if (!strncmp(back,"pitch:",6))
        {
            int sign = (back[6]=='+')?+1:-1;
            int val = atoi(back+7);
            g_Items[index].ang.x += sign * val;
            TeleportLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }
        if (!strncmp(back,"roll:",5))
        {
            int sign = (back[5]=='+')?+1:-1;
            int val = atoi(back+6);
            g_Items[index].ang.z += sign * val;
            TeleportLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }
		
        if (!strncmp(back,"scale:",6))
        {
            if (!strcmp(back, "scale:reset"))
            {
                g_Items[index].scale = 1.0f;
            }
            else
            {
                float delta = (float)atof(back + 6);
                g_Items[index].scale = ClampScale(g_Items[index].scale + delta);
            }
            ApplyVisualScaleToLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }

        if (!strcmp(back, "invis:on"))
        {
            g_Items[index].invisible = true;
            ApplyInvisibilityToLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }
        if (!strcmp(back, "invis:off"))
        {
            g_Items[index].invisible = false;
            ApplyInvisibilityToLive(index);
            OpenRotateMenu(iSlot, index);
            return;
        }
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
    GET_V_IFACE_ANY    (GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

    g_SMAPI->AddListener(this, this);
    return true;
}

bool BlockerPasses::Unload(char *error, size_t maxlen)
{
    ConVar_Unregister();
    if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);
    ClearLive(true);
    return true;
}

void BlockerPasses::AllPluginsLoaded()
{
    char error[64]; int ret;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
        ConColorMsg(Color(255,0,0,255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pPlayers)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
        ConColorMsg(Color(255,0,0,255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Menus system plugin");
        ConColorMsg(Color(255,0,0,255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    g_pAdmin = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pAdmin)
    {
        g_SMAPI->Format(error, sizeof(error), "Missing Admin system plugin");
        ConColorMsg(Color(255,0,0,255), "[%s] %s\n", GetLogTag(), error);
        std::string s = "meta unload " + std::to_string(g_PLID);
        if (engine) engine->ServerCommand(s.c_str());
        return;
    }

    LoadSettings();
    LoadPhrases();

    g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pUtils->MapStartHook(g_PLID, OnMapStart);
    g_pUtils->MapEndHook(g_PLID, OnMapEnd);
    g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStartEvent);

    g_pUtils->RegCommand(g_PLID, {"mm_bp"}, {"!bp"}, OnBpCmd);
}

const char* BlockerPasses::GetLicense()
{
    return "GPL";
}

const char* BlockerPasses::GetVersion()
{
    return "1.0";
}

const char* BlockerPasses::GetDate()
{
    return __DATE__;
}

const char *BlockerPasses::GetLogTag()
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
