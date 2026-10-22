// internal ESP, Unity il2cpp, android arm64. everything resolved by name at runtime (no RVAs).
#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <cctype>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <mutex>
#include <vector>
#include <utility>
#include <unordered_map>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
extern "C" void A64HookFunction(void* symbol, void* replace, void** result); // And64InlineHook
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ESP", __VA_ARGS__)

// ---------- il2cpp api ----------
struct Il2CppDomain; struct Il2CppAssembly; struct Il2CppImage; struct Il2CppClass;
struct Il2CppObject; struct Il2CppString; struct Il2CppType; struct FieldInfo;
struct MethodInfo { void* methodPointer; };
struct Il2CppArray { void* k; void* mon; void* bounds; uintptr_t len; void* items[1]; };

#define API(ret, name, args) static ret (*name) args
API(Il2CppDomain*, il2cpp_domain_get, ());
API(Il2CppAssembly**, il2cpp_domain_get_assemblies, (Il2CppDomain*, size_t*));
API(Il2CppImage*, il2cpp_assembly_get_image, (Il2CppAssembly*));
API(Il2CppClass*, il2cpp_class_from_name, (Il2CppImage*, const char*, const char*));
API(MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass*, const char*, int));
API(FieldInfo*, il2cpp_class_get_field_from_name, (Il2CppClass*, const char*));
API(size_t, il2cpp_field_get_offset, (FieldInfo*));
API(void*, il2cpp_thread_attach, (Il2CppDomain*));
API(const Il2CppType*, il2cpp_class_get_type, (Il2CppClass*));
API(Il2CppObject*, il2cpp_type_get_object, (const Il2CppType*));
API(Il2CppObject*, il2cpp_runtime_invoke, (MethodInfo*, void*, void**, Il2CppObject**));
API(Il2CppClass*, il2cpp_object_get_class, (Il2CppObject*));
API(const char*, il2cpp_class_get_name, (Il2CppClass*));
API(Il2CppString*, il2cpp_string_new, (const char*));
API(uint32_t, il2cpp_gchandle_new, (Il2CppObject*, bool));
API(void, il2cpp_gchandle_free, (uint32_t));

static Il2CppDomain* g_dom;

static bool ResolveApi(void* h) {
#define R(n) if (!(n = (decltype(n))dlsym(h, #n))) return false;
    R(il2cpp_domain_get) R(il2cpp_domain_get_assemblies) R(il2cpp_assembly_get_image)
    R(il2cpp_class_from_name) R(il2cpp_class_get_method_from_name) R(il2cpp_class_get_field_from_name)
    R(il2cpp_field_get_offset) R(il2cpp_thread_attach) R(il2cpp_class_get_type) R(il2cpp_type_get_object)
    R(il2cpp_runtime_invoke) R(il2cpp_object_get_class) R(il2cpp_class_get_name) R(il2cpp_string_new) R(il2cpp_gchandle_new) R(il2cpp_gchandle_free)
#undef R
    return true;
}

static Il2CppClass* Cls(const char* ns, const char* n) {
    size_t c = 0; auto as = il2cpp_domain_get_assemblies(g_dom, &c);
    for (size_t i = 0; i < c; i++)
        if (auto k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, n)) return k;
    return nullptr;
}
static MethodInfo* Meth(Il2CppClass* k, const char* n, int argc) { return k ? il2cpp_class_get_method_from_name(k, n, argc) : nullptr; }
static int Off(Il2CppClass* k, const char* n) { auto f = k ? il2cpp_class_get_field_from_name(k, n) : nullptr; return f ? (int)il2cpp_field_get_offset(f) : -1; }
static Il2CppObject* Inv(MethodInfo* m, void* o, void** a = nullptr) {
    if (!m) return nullptr; Il2CppObject* ex = nullptr; auto r = il2cpp_runtime_invoke(m, o, a, &ex); return ex ? nullptr : r;
}
template <class T> static T Unbox(Il2CppObject* o, T d = {}) { return o ? *(T*)((char*)o + 0x10) : d; }
template <class T> static T Fld(void* o, int off) { return *(T*)((char*)o + off); }
static std::string Str(Il2CppString* s) { // utf16 -> utf8
    std::string r; if (!s) return r;
    int n = *(int*)((char*)s + 0x10); auto c = (const char16_t*)((char*)s + 0x14);
    for (int i = 0; i < n; i++) {
        unsigned u = c[i];
        if (u < 0x80) r += (char)u;
        else if (u < 0x800) { r += (char)(0xC0 | u >> 6); r += (char)(0x80 | (u & 63)); }
        else { r += (char)(0xE0 | u >> 12); r += (char)(0x80 | ((u >> 6) & 63)); r += (char)(0x80 | (u & 63)); }
    }
    return r;
}

// ---------- config ----------
struct Cfg {
    bool esp = true, box = true, hp = true, name = true, dist = true, lines = false, team = true, skel = true, noRecoil = false, noSpread = false, saim = false, adraw = true;
    float ecol[4] = {1, .2f, .2f, 1}, tcol[4] = {.2f, .6f, 1, 1};
    float maxd = 200, thick = 2, afov = 10;
    int abone = 0;
} C;
static bool g_menu = true;
static std::string g_cfgMsg = "not saved yet";
static std::vector<std::string> CfgPaths() {
    char b[128] = {}; FILE* f = fopen("/proc/self/cmdline", "r"); if (f) { fread(b, 1, 127, f); fclose(f); }
    std::string pkg = b;
    return {"/data/data/" + pkg + "/files/esp.json", "/storage/emulated/0/Android/data/" + pkg + "/files/esp.json", "/data/data/" + pkg + "/esp.json"};
}
static void Save() {
    char buf[900];
    snprintf(buf, sizeof buf, "{\"esp\":%d,\"box\":%d,\"hp\":%d,\"name\":%d,\"dist\":%d,\"lines\":%d,\"team\":%d,\"skel\":%d,\"norec\":%d,\"nospr\":%d,\"saim\":%d,\"adraw\":%d,\"abone\":%d,\"afov\":%g,\"maxd\":%g,\"thick\":%g,"
             "\"e0\":%g,\"e1\":%g,\"e2\":%g,\"e3\":%g}",
             C.esp, C.box, C.hp, C.name, C.dist, C.lines, C.team, C.skel, C.noRecoil, C.noSpread, C.saim, C.adraw, C.abone, C.afov, C.maxd, C.thick, C.ecol[0], C.ecol[1], C.ecol[2], C.ecol[3]);
    for (auto& p : CfgPaths()) {
        mkdir(p.substr(0, p.rfind('/')).c_str(), 0771);
        FILE* f = fopen(p.c_str(), "w"); if (!f) continue;
        fputs(buf, f); fclose(f); g_cfgMsg = "saved: " + p; LOG("%s", g_cfgMsg.c_str()); return;
    }
    g_cfgMsg = std::string("SAVE FAILED, errno ") + std::to_string(errno); LOG("%s", g_cfgMsg.c_str());
}
static void Load() {
    for (auto& p : CfgPaths()) {
        FILE* f = fopen(p.c_str(), "r"); if (!f) continue;
        char b[1024] = {}; fread(b, 1, 1023, f); fclose(f); std::string s = b;
        auto g = [&](const char* k, float d) { auto q = s.find(std::string("\"") + k + "\":"); return q == std::string::npos ? d : (float)atof(s.c_str() + q + strlen(k) + 3); };
        C.esp = g("esp", 1) != 0; C.box = g("box", 1) != 0; C.hp = g("hp", 1) != 0; C.name = g("name", 1) != 0; C.dist = g("dist", 1) != 0;
        C.lines = g("lines", 0) != 0; C.team = g("team", 1) != 0; C.skel = g("skel", 1) != 0; C.noRecoil = g("norec", 0) != 0; C.noSpread = g("nospr", 0) != 0; C.saim = g("saim", 0) != 0; C.adraw = g("adraw", 1) != 0; C.abone = (int)g("abone", 0); C.afov = g("afov", 10); C.maxd = g("maxd", 200); C.thick = g("thick", 2);
        const char* e[] = {"e0", "e1", "e2", "e3"}; for (int i = 0; i < 4; i++) C.ecol[i] = g(e[i], C.ecol[i]);
        g_cfgMsg = "loaded: " + p; LOG("%s", g_cfgMsg.c_str()); return;
    }
    g_cfgMsg = "no saved config found";
}

// ---------- shared snapshot (game thread writes, render thread reads) ----------
struct Ent { ImVec2 head, feet; int hp; float dist; bool enemy; char name[40]; std::vector<std::pair<ImVec2, ImVec2>> bones; };
static std::mutex g_mx;
static std::vector<Ent> g_ents;
static float g_sw = 0, g_sh = 0;
static void* g_local = nullptr; static int g_myTeam = 0; static float g_vfov = 60.f;
static struct { bool down = false; float x = 0, y = 0; } g_touch; // y raw, bottom-up

// ---------- game side ----------
static Il2CppClass *kPC, *kComp, *kCam, *kTr, *kObj, *kInput, *kPhView, *kPlayer;
static MethodInfo *mCamFov, *mScrW, *mScrH, *mFind, *mGetTr, *mGetPos, *mCamMain, *mW2S, *mTouchCnt, *mGetTouch, *mIsMine, *mHealth,
                  *mNick, *mProps, *mItem;
static int oView, oPlayer, oDead, oCam, oAnim = -1, oSkel = -1, oSkelCt = -1, oSkelTr = -1, oRag = -1, oBone[7] = {-1, -1, -1, -1, -1, -1, -1};
static MethodInfo* mGetParent;
static MethodInfo *mGetBones, *mGetName;
static int oBody = -1, oBodyLod = -1;
static Il2CppObject* g_pcType;
static Il2CppArray* g_arr; static int g_tick;

static int TeamOf(Il2CppObject* player) { // any value type: equal value == same team
    if (!player) return 0;
    auto ht = Inv(mProps, player); if (!ht) return 0;
    auto k = il2cpp_object_get_class(ht);
    static MethodInfo* get = nullptr; if (!get) get = Meth(k, "get_Item", 1);
    void* a[1] = {il2cpp_string_new("team")};
    auto v = Inv(get, ht, a); if (!v) return 0;
    const char* cn = il2cpp_class_get_name(il2cpp_object_get_class(v));
    if (!strcmp(cn, "String")) {
        auto s = Str((Il2CppString*)v); if (s.empty()) return 0;
        unsigned h = 5381; for (unsigned char ch : s) h = h * 33 + ch; return (int)(h | 1);
    }
    if (!strcmp(cn, "Byte")) return Unbox<unsigned char>(v);
    return Unbox<int>(v); // Int32 / Team enum
}
static bool Pos(void* go, float* o) {
    auto tr = Inv(mGetTr, go); if (!tr) return false;
    auto p = Inv(mGetPos, tr); if (!p) return false; memcpy(o, (char*)p + 0x10, 12); return true;
}
static bool W2S(void* cam, float x, float y, float z, ImVec2& out) {
    float v[3] = {x, y, z}; void* a[1] = {v};
    auto r = Inv(mW2S, cam, a); if (!r) return false;
    float* s = (float*)((char*)r + 0x10); if (s[2] <= 0.f) return false;
    out = ImVec2(s[0], s[1]); return true; // y still bottom-up, flipped at draw
}
// skeleton = lines between ragdoll bones, parent link = nearest ancestor transform that is also a bone
// bones we do not want to draw (fingers, toes, twist/helper bones, ik targets...)
static bool SkipBone(void* t) {
    if (!mGetName) return false;
    auto n = (Il2CppString*)Inv(mGetName, t); if (!n) return false;
    std::string s = Str(n); for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    static const char* bad[] = {"finger", "thumb", "index", "middle", "ring", "pinky", "toe", "twist", "roll", "helper", "nub", "eye", "jaw", "tongue", "hair", "ik", "pole", "socket", "weapon", "_end"};
    for (auto b : bad) if (s.find(b) != std::string::npos) return true;
    return false;
}
struct SkelC { void* anim = nullptr; void* skel = nullptr; std::vector<void*> tr; std::vector<int> par; };
static std::unordered_map<void*, SkelC> g_skel;
static void Skel(void* pc, void* cam, Ent& e) {
    if (oAnim < 0 || oSkel < 0) return;
    void* anim = Fld<void*>(pc, oAnim); if (!anim) return;
    void* sk = Fld<void*>(anim, oSkel);
    if (!sk && oSkelCt >= 0) sk = Fld<void*>(anim, oSkelCt);
    if (!sk && oSkelTr >= 0) sk = Fld<void*>(anim, oSkelTr);
    if (!sk) return;
    auto it = g_skel.find(pc);
    if (it == g_skel.end() || it->second.anim != anim || it->second.skel != sk) {   // (re)build the bone list
        if (g_skel.size() > 64) g_skel.clear();
        SkelC sc; sc.anim = anim; sc.skel = sk;
        const char* src = "skinned body";
        {   // main source: bone list of the skinned body mesh = the real armature
            void* body = oBody >= 0 ? Fld<void*>(sk, oBody) : nullptr;
            if (!body && oBodyLod >= 0) body = Fld<void*>(sk, oBodyLod);
            auto ba = (body && mGetBones) ? (Il2CppArray*)Inv(mGetBones, body) : nullptr;
            if (ba) for (uintptr_t i = 0; i < ba->len && i < 150; i++) { void* t = ba->items[i]; if (t && !SkipBone(t)) sc.tr.push_back(t); }
        }
        if (sc.tr.size() < 6) {   // fallback: ragdoll rigidbodies, then the 7 named bones
            sc.tr.clear(); src = "ragdoll";
            std::vector<void*> rbs;
            auto arr = oRag >= 0 ? Fld<Il2CppArray*>(sk, oRag) : nullptr;
            if (arr) for (uintptr_t i = 0; i < arr->len && i < 40; i++) if (arr->items[i]) rbs.push_back(arr->items[i]);
            if (rbs.empty()) { src = "7 named"; for (int k = 0; k < 7; k++) if (oBone[k] >= 0) { void* r = Fld<void*>(sk, oBone[k]); if (r) rbs.push_back(r); } }
            for (void* r : rbs) { auto t = Inv(mGetTr, r); if (t) sc.tr.push_back(t); }
        }
        sc.par.assign(sc.tr.size(), -1);
        auto idxOf = [&](void* p) { for (size_t j = 0; j < sc.tr.size(); j++) if (sc.tr[j] == p) return (int)j; return -1; };
        for (size_t i = 0; i < sc.tr.size(); i++) {
            void* p = sc.tr[i];
            for (int d = 0; d < 14 && p; d++) { p = Inv(mGetParent, p); int j = p ? idxOf(p) : -1; if (j >= 0) { sc.par[i] = j; break; } }
        }
        { int links = 0; for (int p : sc.par) if (p >= 0) links++; LOG("skeleton built: bones=%lu links=%d from=%s", (unsigned long)sc.tr.size(), links, src); }
        it = g_skel.insert_or_assign(pc, std::move(sc)).first;
    }
    auto& sc = it->second;
    std::vector<ImVec2> sp(sc.tr.size()); std::vector<char> ok(sc.tr.size(), 0);
    for (size_t i = 0; i < sc.tr.size(); i++) {
        auto pv = Inv(mGetPos, sc.tr[i]); if (!pv) continue;
        float* v = (float*)((char*)pv + 0x10); ok[i] = W2S(cam, v[0], v[1], v[2], sp[i]);
    }
    for (size_t i = 0; i < sc.tr.size(); i++) if (sc.par[i] >= 0 && ok[i] && ok[sc.par[i]]) e.bones.push_back({sp[sc.par[i]], sp[i]});
}
static int HP(Il2CppObject* o) {
    if (!o) return 100; const char* n = il2cpp_class_get_name(il2cpp_object_get_class(o));
    if (!strcmp(n, "Int32")) return Unbox<int>(o); if (!strcmp(n, "Single")) return (int)Unbox<float>(o); return 100;
}
static uint32_t g_arrH;
static void Collect() {
    static int logn = 0; void* dbgLocal = nullptr; void* dbgCam = nullptr; int hidden = 0, dbgTeam = 0;
    if (g_pcType && (!g_arr || (g_tick++ % 30) == 0)) {
        bool inc = false; void* a[2] = {g_pcType, &inc};
        auto arr = (Il2CppArray*)Inv(mFind, nullptr, a);
        if (g_arrH) il2cpp_gchandle_free(g_arrH);          // keep the array alive for the GC while we cache it
        g_arrH = arr ? il2cpp_gchandle_new((Il2CppObject*)arr, false) : 0; g_arr = arr;
    }
    std::vector<Ent> out;
    if (g_arr) {
        void* local = nullptr; int myTeam = 0; float lp[3] = {}; bool haveLp = false;
        for (uintptr_t i = 0; i < g_arr->len; i++) {
            void* pc = g_arr->items[i]; if (!pc) continue;
            auto view = Fld<Il2CppObject*>(pc, oView);
            if (view && Unbox<bool>(Inv(mIsMine, view))) { local = pc; myTeam = TeamOf(Fld<Il2CppObject*>(pc, oPlayer)); haveLp = Pos(pc, lp); break; }
        }
        Il2CppObject* cam = (local && oCam >= 0) ? Fld<Il2CppObject*>(local, oCam) : nullptr;
        if (!cam) cam = Inv(mCamMain, nullptr);
        if (cam && !haveLp) haveLp = Pos(cam, lp);           // no local player found: measure from the camera
        dbgLocal = local; dbgCam = cam; dbgTeam = myTeam; g_local = local; g_myTeam = myTeam;
        if (cam && mCamFov) g_vfov = Unbox<float>(Inv(mCamFov, cam), 60.f);
        if (cam && haveLp) for (uintptr_t i = 0; i < g_arr->len; i++) {
            void* pc = g_arr->items[i]; if (!pc || pc == local) continue;
            if (Fld<bool>(pc, oDead)) continue;
            float p[3]; if (!Pos(pc, p)) continue;
            float dist = sqrtf(powf(p[0] - lp[0], 2) + powf(p[1] - lp[1], 2) + powf(p[2] - lp[2], 2));
            if (!local && dist < 1.5f) continue;             // fallback mode: skip our own body
            Ent e{}; e.dist = dist;
            if (!W2S(cam, p[0], p[1], p[2], e.feet) || !W2S(cam, p[0], p[1] + 1.8f, p[2], e.head)) continue;
            e.hp = HP(Inv(mHealth, pc));
            auto pl = Fld<Il2CppObject*>(pc, oPlayer);
            int t = TeamOf(pl); e.enemy = true;
            if (C.team && myTeam && t == myTeam) { hidden++; continue; }   // teammate
            snprintf(e.name, sizeof e.name, "%s", Str((Il2CppString*)Inv(mNick, pl)).c_str());
            if (C.skel) Skel(pc, cam, e);
            out.push_back(e);
        }
    }
    if ((logn++ % 250) == 0) LOG("collect: players=%lu local=%p cam=%p drawn=%lu myTeam=%d hiddenTeam=%d", g_arr ? (unsigned long)g_arr->len : 0UL, dbgLocal, dbgCam, (unsigned long)out.size(), dbgTeam, hidden);
    // touch (legacy Input, main thread only)
    static float tx = 0, ty = 0; bool down = false;
    int n = Unbox<int>(Inv(mTouchCnt, nullptr));
    if (n > 0) { int idx = 0; void* a[1] = {&idx}; auto t = Inv(mGetTouch, nullptr, a);
        static int dbg = 0; if (dbg++ < 5) LOG("touch n=%d obj=%p", n, (void*)t);
        if (t) { float* f = (float*)((char*)t + 0x10); tx = f[1]; ty = f[2]; int ph = *(int*)&f[9]; down = ph != 3 && ph != 4; } }
    float sw = (float)Unbox<int>(Inv(mScrW, nullptr)), sh = (float)Unbox<int>(Inv(mScrH, nullptr));
    std::lock_guard<std::mutex> l(g_mx); g_ents.swap(out); g_touch = {down, tx, ty}; g_sw = sw; g_sh = sh;
}
static void (*orig_update)(void*, MethodInfo*); static MethodInfo* mUpdate;
static void (*orig_es)(void*, MethodInfo*); static MethodInfo* mEs;
static void Tick() { // called from Unity script thread (hooks), ~120Hz max
    static auto last = std::chrono::steady_clock::now(); auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(8)) { last = now; Collect(); }
}
static void hook_update(void* self, MethodInfo* mi) { Tick(); orig_update(self, mi); }
static void hook_es(void* self, MethodInfo* mi) { Tick(); orig_es(self, mi); } // EventSystem.Update: runs in lobby too, keeps touch alive

// ---------- render side ----------
static void Draw(int w, int h, float sx, float sy) {
    auto dl = ImGui::GetBackgroundDrawList();
    std::vector<Ent> es; { std::lock_guard<std::mutex> l(g_mx); es = g_ents; }
    for (auto& e : es) {
        if (e.dist > C.maxd) continue;
        float fy = h - e.feet.y * sy, hy = h - e.head.y * sy, bh = fy - hy, bw = bh * .5f, cx = e.head.x * sx;
        if (bh < 4) continue;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(*(ImVec4*)(e.enemy ? C.ecol : C.tcol));
        if (C.lines) dl->AddLine(ImVec2(w * .5f, (float)h), ImVec2(cx, fy), col, C.thick);
        if (C.skel) for (auto& b : e.bones)
            dl->AddLine(ImVec2(b.first.x * sx, h - b.first.y * sy), ImVec2(b.second.x * sx, h - b.second.y * sy), col, C.thick);
        if (C.box) dl->AddRect(ImVec2(cx - bw / 2, hy), ImVec2(cx + bw / 2, fy), col, 0, 0, C.thick);
        if (C.hp) {
            float f = fminf(fmaxf(e.hp / 100.f, 0), 1), x = cx - bw / 2 - 6;
            dl->AddRectFilled(ImVec2(x - 2, hy), ImVec2(x + 2, fy), IM_COL32(0, 0, 0, 160));
            dl->AddRectFilled(ImVec2(x - 2, fy - bh * f), ImVec2(x + 2, fy), IM_COL32((int)(255 * (1 - f)), (int)(255 * f), 0, 255));
        }
        if (C.name) dl->AddText(ImVec2(cx - ImGui::CalcTextSize(e.name).x / 2, hy - ImGui::GetFontSize() - 2), col, e.name);
        if (C.dist) { char b[16]; snprintf(b, 16, "%dm", (int)e.dist); dl->AddText(ImVec2(cx - ImGui::CalcTextSize(b).x / 2, fy + 2), col, b); }
    }
}
static void Menu() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.88f);
    ImGui::Begin("esp", &g_menu, ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("t")) {
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Checkbox("enable", &C.esp); ImGui::Checkbox("box", &C.box); ImGui::Checkbox("skeleton", &C.skel); ImGui::Checkbox("health", &C.hp);
            ImGui::Checkbox("name", &C.name); ImGui::Checkbox("distance", &C.dist); ImGui::Checkbox("snaplines", &C.lines);
            ImGui::Checkbox("hide teammates", &C.team); ImGui::SliderFloat("max dist", &C.maxd, 20, 500); ImGui::SliderFloat("thickness", &C.thick, 1, 5);
            ImGui::ColorEdit4("enemy", C.ecol, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AIM")) {
            ImGui::Checkbox("silent aim", &C.saim); ImGui::Checkbox("draw fov", &C.adraw);
            ImGui::SliderFloat("fov", &C.afov, 1, 90, "%.0f deg");
            ImGui::TextUnformatted("hit place:");
            ImGui::RadioButton("head", &C.abone, 0); ImGui::SameLine(); ImGui::RadioButton("chest", &C.abone, 1);
            ImGui::RadioButton("pelvis", &C.abone, 2); ImGui::SameLine(); ImGui::RadioButton("legs", &C.abone, 3);
            ImGui::RadioButton("closest to crosshair", &C.abone, 4);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MEMORY")) {
            ImGui::Checkbox("anti recoil", &C.noRecoil); ImGui::Checkbox("anti scatter", &C.noSpread);
            ImGui::TextWrapped("recoil: camera kick and spray pattern. scatter: random bullet spread.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CONFIG")) {
            if (ImGui::Button("SAVE")) Save(); ImGui::SameLine(); if (ImGui::Button("LOAD")) Load(); ImGui::SameLine();
            if (ImGui::Button("RESET")) { C = Cfg(); g_cfgMsg = "defaults restored"; }
            ImGui::TextWrapped("%s", g_cfgMsg.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
// floating bubble: drag anywhere on screen, tap toggles the menu
static void Bubble(float w, float h) {
    const float R = 70.f; static ImVec2 pos(w - 2 * R - 30.f, 160.f); static bool moved = false;
    auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(ImVec2(2 * R, 2 * R));
    ImGui::Begin("##bubble", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::InvisibleButton("b", ImVec2(2 * R, 2 * R));
    if (ImGui::IsItemActivated()) moved = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 12.f)) {
        moved = true; pos.x += io.MouseDelta.x; pos.y += io.MouseDelta.y;
        pos.x = fminf(fmaxf(pos.x, 0), w - 2 * R); pos.y = fminf(fmaxf(pos.y, 0), h - 2 * R);
    }
    if (ImGui::IsItemDeactivated() && !moved) g_menu = !g_menu;
    auto dl = ImGui::GetWindowDrawList(); ImVec2 c(pos.x + R, pos.y + R);
    dl->AddCircleFilled(c, R, g_menu ? IM_COL32(60, 140, 255, 220) : IM_COL32(30, 30, 30, 200), 48);
    dl->AddCircle(c, R, IM_COL32(255, 255, 255, 200), 48, 3.f);
    ImVec2 ts = ImGui::CalcTextSize("ESP"); dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32_WHITE, "ESP");
    ImGui::End();
}
static bool g_init; static int g_w, g_h;
static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);
static EGLBoolean hook_swap(EGLDisplay d, EGLSurface s) {
    EGLint w = 0, h = 0; eglQuerySurface(d, s, EGL_WIDTH, &w); eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_swap(d, s);
    if (!g_init) {
        ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        ImGui::StyleColorsDark(); ImGui::GetStyle().ScaleAllSizes(3.f); io.FontGlobalScale = 2.6f;
        ImGui_ImplOpenGL3_Init("#version 300 es"); Load(); g_init = true; LOG("imgui init %dx%d", w, h);
    }
    auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2((float)w, (float)h);
    float sx = 1, sy = 1;
    { std::lock_guard<std::mutex> l(g_mx); if (g_sw > 1) sx = w / g_sw; if (g_sh > 1) sy = h / g_sh;
      io.AddMousePosEvent(g_touch.x * sx, h - g_touch.y * sy); io.AddMouseButtonEvent(0, g_touch.down); }
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    if (C.esp) Draw(w, h, sx, sy);
    if (C.saim && C.adraw) {   // fov circle: angular radius -> pixels through the camera fov
        float r = tanf(C.afov * 0.0174533f) / tanf(g_vfov * 0.5f * 0.0174533f) * (h * 0.5f); if (r > h) r = (float)h;
        ImGui::GetBackgroundDrawList()->AddCircle(ImVec2(w * .5f, h * .5f), r, IM_COL32(255, 255, 255, 170), 64, 2.f);
    }
    Bubble((float)w, (float)h);
    if (g_menu) Menu();
    ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_swap(d, s);
}

// ---------- bootstrap ----------
// ---------- AIM: silent aim = redirect the shot direction, the camera never moves ----------
struct V3 { float x, y, z; }; struct Q4 { float x, y, z, w; };
static V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 Norm(V3 a) { float l = sqrtf(Dot(a, a)); return l > 1e-6f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1}; }
static Q4 LookRot(V3 f) { // unity LookRotation(f, up)
    V3 r = Cross(V3{0, 1, 0}, f); if (Dot(r, r) < 1e-6f) r = V3{1, 0, 0}; r = Norm(r); V3 u = Cross(f, r);
    float m00 = r.x, m01 = u.x, m02 = f.x, m10 = r.y, m11 = u.y, m12 = f.y, m20 = r.z, m21 = u.z, m22 = f.z, tr = m00 + m11 + m22; Q4 q;
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, .25f * s}; }
    else if (m00 > m11 && m00 > m22) { float s = sqrtf(1 + m00 - m11 - m22) * 2; q = {.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
    else if (m11 > m22) { float s = sqrtf(1 + m11 - m00 - m22) * 2; q = {(m01 + m10) / s, .25f * s, (m12 + m21) / s, (m02 - m20) / s}; }
    else { float s = sqrtf(1 + m22 - m00 - m11) * 2; q = {(m02 + m20) / s, (m12 + m21) / s, .25f * s, (m10 - m01) / s}; }
    return q;
}
static void* SkelOfPc(void* pc) {
    if (oAnim < 0 || oSkel < 0) return nullptr;
    void* anim = Fld<void*>(pc, oAnim); if (!anim) return nullptr;
    void* sk = Fld<void*>(anim, oSkel);
    if (!sk && oSkelCt >= 0) sk = Fld<void*>(anim, oSkelCt);
    if (!sk && oSkelTr >= 0) sk = Fld<void*>(anim, oSkelTr);
    return sk;
}
static bool BoneAt(void* pc, int k, V3* o) { // k = index into oBone: 0 head 1 chest 2 pelvis 5/6 thighs
    float p[3]; void* sk = SkelOfPc(pc);
    if (sk && oBone[k] >= 0) { void* rb = Fld<void*>(sk, oBone[k]); if (rb && Pos(rb, p)) { *o = {p[0], p[1], p[2]}; return true; } }
    if (!Pos(pc, p)) return false;                                   // fallback: feet + fixed height
    static const float hgt[7] = {1.7f, 1.3f, 0.95f, 1.3f, 1.3f, 0.5f, 0.5f};
    *o = {p[0], p[1] + hgt[k], p[2]}; return true;
}
static bool PickTarget(V3 cam, V3 fwd, V3* out) {
    if (!g_arr || !g_local) return false;
    fwd = Norm(fwd); float best = C.afov; bool ok = false;
    for (uintptr_t i = 0; i < g_arr->len; i++) {
        void* pc = g_arr->items[i]; if (!pc || pc == g_local || Fld<bool>(pc, oDead)) continue;
        if (C.team && g_myTeam && TeamOf(Fld<Il2CppObject*>(pc, oPlayer)) == g_myTeam) continue;   // never aim at teammates
        V3 cand[3]; int n = 0, a = C.abone;
        if (a == 0 || a == 4) { if (BoneAt(pc, 0, &cand[n])) n++; }
        if (a == 1 || a == 4) { if (BoneAt(pc, 1, &cand[n])) n++; }
        if (a == 2 || a == 4) { if (BoneAt(pc, 2, &cand[n])) n++; }
        if (a == 3) { V3 l, r; if (BoneAt(pc, 5, &l)) { cand[n] = l; if (BoneAt(pc, 6, &r)) cand[n] = {(l.x + r.x) / 2, (l.y + r.y) / 2, (l.z + r.z) / 2}; n++; } }
        for (int j = 0; j < n; j++) {
            V3 d = Sub(cand[j], cam); if (sqrtf(Dot(d, d)) > C.maxd) continue;
            float c = fminf(fmaxf(Dot(Norm(d), fwd), -1.f), 1.f), ang = acosf(c) * 57.29578f;
            if (ang < best) { best = ang; *out = cand[j]; ok = true; }
        }
    }
    return ok;
}
static void (*o_fr)(void*, V3, V3, float, bool, MethodInfo*);            // ArmsController.FireRay(camPos, forward, distance, isKnife)
static void h_fr(void* s, V3 cam, V3 fwd, float dist, bool knife, MethodInfo* mi) {
    V3 t; if (C.saim && !knife && PickTarget(cam, fwd, &t)) fwd = Norm(Sub(t, cam));
    o_fr(s, cam, fwd, dist, knife, mi);
}
static void (*o_fp)(void*, void*, V3, Q4, float, int, MethodInfo*);     // ArmsController.FirePellets(params, origin, aim, distance, pellets)
static void h_fp(void* s, void* prm, V3 org, Q4 aim, float dist, int n, MethodInfo* mi) {
    if (C.saim) {
        V3 f = {2 * (aim.x * aim.z + aim.w * aim.y), 2 * (aim.y * aim.z - aim.w * aim.x), 1 - 2 * (aim.x * aim.x + aim.y * aim.y)}, t;
        if (PickTarget(org, f, &t)) aim = LookRot(Norm(Sub(t, org)));
    }
    o_fp(s, prm, org, aim, dist, n, mi);
}

// ---------- MEMORY: anti recoil / anti scatter (hooks on ArmsController / RecoilControl, output zeroed in RecoilData) ----------
static int oXd = -1, oYd = -1, oAng = -1, oRel = -1;
static void ZeroRD(void* r) {
    if (!r) return; char* c = (char*)r;
    if (C.noRecoil) { if (oXd >= 0) *(float*)(c + oXd) = 0; if (oYd >= 0) *(float*)(c + oYd) = 0; }
    if (C.noSpread) { if (oAng >= 0) *(float*)(c + oAng) = 0; if (oRel >= 0) { *(float*)(c + oRel) = 0; *(float*)(c + oRel + 4) = 0; } }
}
static void (*o_acr)(void*, float, MethodInfo*);                        // ApplyCameraRecoil(float coeff)
static void h_acr(void* s, float c, MethodInfo* mi) { o_acr(s, C.noRecoil ? 0.f : c, mi); }
static void (*o_grd)(void*, void**, float, float, int, MethodInfo*);   // GetRelativeDispertion(ref RecoilData, add, angle, edgeShots)
static void h_grd(void* s, void** rd, float add, float ang, int edge, MethodInfo* mi) {
    if (C.noSpread) { add = 0; ang = 0; }
    o_grd(s, rd, add, ang, edge, mi); if (rd) ZeroRD(*rd);
}
static void* (*o_gns)(void*, bool, float, MethodInfo*);                 // RecoilControl.GetNextShot(bool, float)
static void* h_gns(void* s, bool cont, float t, MethodInfo* mi) { void* r = o_gns(s, cont, t, mi); ZeroRD(r); return r; }
static void* (*o_gcd)(void*, float, MethodInfo*);                       // RecoilControl.GetCurrentDeviation(float)
static void* h_gcd(void* s, float t, MethodInfo* mi) { void* r = o_gcd(s, t, mi); ZeroRD(r); return r; }

static void* Boot(void*) {
    LOG("boot");
    void* egl = dlsym(dlopen("libEGL.so", RTLD_NOW), "eglSwapBuffers");
    if (egl) { A64HookFunction(egl, (void*)hook_swap, (void**)&orig_swap); LOG("swap hooked %p", egl); } else LOG("eglSwapBuffers not found");
    void* h = nullptr;
    while (!(h = dlopen("libil2cpp.so", RTLD_NOLOAD)) || !ResolveApi(h)) sleep(1);
    LOG("il2cpp api ok");
    while (!(g_dom = il2cpp_domain_get())) sleep(1);
    il2cpp_thread_attach(g_dom);
    for (int i = 0; i < 60 && !Cls("", "PlayerController"); i++) sleep(1); // wait until game code is loaded, not a fixed delay
    kPC = Cls("", "PlayerController"); kComp = Cls("UnityEngine", "Component"); kCam = Cls("UnityEngine", "Camera");
    kTr = Cls("UnityEngine", "Transform"); kObj = Cls("UnityEngine", "Object"); kInput = Cls("UnityEngine", "Input");
    kPhView = Cls("Photon.Pun", "PhotonView"); kPlayer = Cls("Photon.Realtime", "Player");
    mFind = Meth(kObj, "FindObjectsOfType", 2); mGetTr = Meth(kComp, "get_transform", 0); mGetPos = Meth(kTr, "get_position", 0); mGetParent = Meth(kTr, "get_parent", 0);
    mCamMain = Meth(kCam, "get_main", 0); mCamFov = Meth(kCam, "get_fieldOfView", 0); mW2S = Meth(kCam, "WorldToScreenPoint", 1);
    { auto kScr = Cls("UnityEngine", "Screen"); mScrW = Meth(kScr, "get_width", 0); mScrH = Meth(kScr, "get_height", 0); }
    mTouchCnt = Meth(kInput, "get_touchCount", 0); mGetTouch = Meth(kInput, "GetTouch", 1);
    mIsMine = Meth(kPhView, "get_IsMine", 0); mHealth = Meth(kPC, "get_Health", 0);
    mNick = Meth(kPlayer, "get_NickName", 0); mProps = Meth(kPlayer, "get_CustomProperties", 0);
    oView = Off(kPC, "_view"); oPlayer = Off(kPC, "_player"); oDead = Off(kPC, "_isDeath"); oCam = Off(kPC, "_mainCamera");
    { auto kPAC = Cls("ScarletWork.Photon.TPS", "PlayerAnimatorController"); auto kSk = Cls("ScarletWork.Game", "PlayerSkeleton");   // namespaces from the dump
      oAnim = Off(kPC, "_animatorController"); oSkel = Off(kPAC, "_skeleton"); oSkelCt = Off(kPAC, "_ctSkeleton"); oSkelTr = Off(kPAC, "_trSkeleton");
      oRag = Off(kSk, "ragdollRigidbodies"); oBody = Off(kSk, "_body"); oBodyLod = Off(kSk, "_bodyLod");
      mGetBones = Meth(Cls("UnityEngine", "SkinnedMeshRenderer"), "get_bones", 0); mGetName = Meth(kObj, "get_name", 0);
      const char* bn[7] = {"_headBone", "_chestBone", "_pelvisBone", "_leftUpperArm", "_rightUpperArm", "_leftThigh", "_rightThigh"};
      for (int i = 0; i < 7; i++) oBone[i] = Off(kSk, bn[i]);
      LOG("skeleton offsets anim=%d skel=%d rag=%d body=%d/%d parent=%p bones=%p name=%p", oAnim, oSkel, oRag, oBody, oBodyLod, (void*)mGetParent, (void*)mGetBones, (void*)mGetName); }
    LOG("resolve PC=%p find=%p w2s=%p touch=%p/%p isMine=%p nick=%p props=%p off=%d/%d/%d",
        kPC, mFind, mW2S, mTouchCnt, mGetTouch, mIsMine, mNick, mProps, oView, oPlayer, oDead);
    if (!kPC || !mFind || !mW2S || oView < 0 || oPlayer < 0 || oDead < 0) { LOG("resolve failed, menu only"); }
    else g_pcType = il2cpp_type_get_object(il2cpp_class_get_type(kPC));
    mEs = Meth(Cls("UnityEngine.EventSystems", "EventSystem"), "Update", 0);
    if (mEs) { A64HookFunction(mEs->methodPointer, (void*)hook_es, (void**)&orig_es); LOG("EventSystem.Update hooked"); }
    mUpdate = Meth(kPC, "Update", 0);
    if (mUpdate) { A64HookFunction(mUpdate->methodPointer, (void*)hook_update, (void**)&orig_update); LOG("PlayerController.Update hooked"); }
    { auto kRD = Cls("", "RecoilData"); oXd = Off(kRD, "XDeviation"); oYd = Off(kRD, "YDeviation"); oAng = Off(kRD, "AngleDispertion"); oRel = Off(kRD, "RelativeDispersion");
      auto kArms = Cls("", "ArmsController"); auto kRC = Cls("", "RecoilControl");
      auto a = Meth(kArms, "ApplyCameraRecoil", 1); auto b = Meth(kArms, "GetRelativeDispertion", 4);
      auto c = Meth(kRC, "GetNextShot", 2); auto d = Meth(kRC, "GetCurrentDeviation", 1);
      if (a) A64HookFunction(a->methodPointer, (void*)h_acr, (void**)&o_acr);
      if (b) A64HookFunction(b->methodPointer, (void*)h_grd, (void**)&o_grd);
      if (c) A64HookFunction(c->methodPointer, (void*)h_gns, (void**)&o_gns);
      if (d) A64HookFunction(d->methodPointer, (void*)h_gcd, (void**)&o_gcd);
      LOG("memory hooks: acr=%p grd=%p gns=%p gcd=%p fields=%d/%d/%d/%d", (void*)a, (void*)b, (void*)c, (void*)d, oXd, oYd, oAng, oRel); }
    { auto kArms = Cls("", "ArmsController"); auto fr = Meth(kArms, "FireRay", 4); auto fp = Meth(kArms, "FirePellets", 5);
      if (fr) A64HookFunction(fr->methodPointer, (void*)h_fr, (void**)&o_fr);
      if (fp) A64HookFunction(fp->methodPointer, (void*)h_fp, (void**)&o_fp);
      LOG("aim hooks: fireRay=%p firePellets=%p", (void*)fr, (void*)fp); }
    return nullptr;
}
static void Start() { static bool once; if (once) return; once = true; pthread_t t; pthread_create(&t, nullptr, Boot, nullptr); }
__attribute__((constructor)) static void Ctor() { Start(); }               // LD_PRELOAD path
JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) { Start(); return JNI_VERSION_1_6; } // System.loadLibrary path
