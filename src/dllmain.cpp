#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mmsystem.h>

#include "minhook/MinHook.h"
#include "embedded_neverlose_sound.hpp"

static HMODULE g_self_module = nullptr;
static HANDLE  g_log = INVALID_HANDLE_VALUE;

static void log_init() {
    char path[MAX_PATH];
    if (GetModuleFileNameA(g_self_module, path, sizeof(path))) {
        char* last = strrchr(path, '\\');
        if (last) {
            strcpy(last + 1, "qernix.log");
            g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
}

static void log_write(const char* msg) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(g_log, msg, (DWORD)strlen(msg), &w, nullptr);
    WriteFile(g_log, "\r\n", 2, &w, nullptr);
    FlushFileBuffers(g_log);
}

static void log_fmt(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_write(buf);
}

static uintptr_t pattern_scan(const char* mod_name, const char* pattern, size_t len) {
    HMODULE mod = GetModuleHandleA(mod_name);
    if (!mod) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uintptr_t)mod + dos->e_lfanew);
    uintptr_t start = (uintptr_t)mod;
    uintptr_t end   = start + nt->OptionalHeader.SizeOfImage;
    for (uintptr_t i = start; i < end - len; ++i) {
        bool ok = true;
        for (size_t j = 0; j < len; ++j) {
            if (pattern[j] == '?') continue;
            if (*(uint8_t*)(i + j) != (uint8_t)pattern[j]) { ok = false; break; }
        }
        if (ok) return i;
    }
    return 0;
}

static uintptr_t pattern_scan_str(const char* mod, const char* s) {
    size_t slen = strlen(s);
    char* bytes = (char*)malloc(slen);
    if (!bytes) return 0;
    size_t j = 0;
    for (size_t i = 0; i < slen; ++i) {
        if (s[i] == ' ') continue;
        if (s[i] == '?') {
            bytes[j++] = '?';
            if (s[i+1] == '?') ++i;
        } else {
            bytes[j++] = (char)strtol(s + i, nullptr, 16);
            ++i;
        }
    }
    uintptr_t result = pattern_scan(mod, bytes, j);
    free(bytes);
    return result;
}

struct c_base_handle {
    uint32_t m_index;
    uint32_t m_serial;
    bool is_valid() const { return m_index != 0xFFFFFFFF; }
    int  get_entry_index() const { return m_index & 0x7FFF; }
};

struct c_material_2 {
    uint8_t pad[0x38];
    char*   m_name;
};

struct c_mesh_primitive {
    uint8_t  pad_00[24];
    void*    m_scene_animatable_object;
    c_material_2* m_material;
    c_material_2* m_material2;
    uint8_t  pad_01[32];
    uint8_t  m_color_r, m_color_g, m_color_b, m_color_a;
    uint8_t  pad_02[24];
};

struct c_mesh_primitive_output_buffer {
    c_mesh_primitive* m_prims;
    uint8_t  pad_00[4];
    int      m_arr_size;
    c_mesh_primitive* get_primitive(int i) { return &m_prims[i]; }
};

struct c_scene_animatable_object {
    uint8_t     pad_00[0x78];
    uint32_t    m_flags;
    uint8_t     pad_01[0x44];
    c_base_handle m_owner;
    uint8_t     pad_02[0x88];
};

struct c_animatable_scene_object_desc {
    uint8_t pad_00[0x10];
};

struct kv3_id_t {
    const char*   m_name;
    uint64_t      m_unk0;
    uint64_t      m_unk1;
};

#define STRINGTOKEN_MURMURHASH_SEED 0x31415926

static uint32_t murmur_hash2(const void* key, int len, uint32_t seed) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = seed ^ len;
    const unsigned char* data = (const unsigned char*)key;
    while (len >= 4) {
        uint32_t k = *(uint32_t*)data;
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= data[2] << 16; [[fallthrough]];
        case 2: h ^= data[1] << 8; [[fallthrough]];
        case 1: h ^= data[0]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

#define TOLOWERU(c) ((uint32_t)(((c >= 'A') && (c <= 'Z')) ? c + 32 : c))

static uint32_t murmur_hash2_lower(const char* str, int len, uint32_t seed) {
    char* p = (char*)malloc(len + 1);
    for (int i = 0; i < len; i++) p[i] = (char)TOLOWERU(str[i]);
    p[len] = 0;
    uint32_t result = murmur_hash2(p, len, seed);
    free(p);
    return result;
}

struct CUtlStringToken {
    uint32_t m_nHashCode;
    uint32_t pad;
    const char* m_szDebugName;

    CUtlStringToken() : m_nHashCode(0), pad(0xFFFFFFFF), m_szDebugName(nullptr) {}
    CUtlStringToken(const char* szString) {
        m_nHashCode = murmur_hash2_lower(szString, (int)strlen(szString), STRINGTOKEN_MURMURHASH_SEED);
        pad = 0xFFFFFFFF;
        m_szDebugName = szString;
    }
};
static_assert(sizeof(CUtlStringToken) == 16, "CUtlStringToken must be exactly 16 bytes");

static constexpr uint32_t fnv1a_hash(const char* str) {
    uint32_t hash = 0x811C9DC5u;
    while (*str) {
        hash = (hash ^ (uint8_t)*str) * 0x01000193u;
        ++str;
    }
    return hash;
}

namespace event_hashes {
    constexpr uint32_t player_hurt = fnv1a_hash("player_hurt");
}

static uintptr_t g_client_base = 0;
static void*     g_entity_system  = nullptr;

static c_material_2* g_mat_visible  = nullptr;
static c_material_2* g_mat_occluded = nullptr;

static void* g_og_generate_primitives = nullptr;
static void* g_og_draw_light_scene    = nullptr;
static void* g_og_present             = nullptr;

using get_entity_fn = void*(__fastcall*)(void*, int);
static get_entity_fn g_get_base_entity = nullptr;

static ID3D11Device*        g_d3d_dev  = nullptr;
static ID3D11DeviceContext* g_d3d_ctx  = nullptr;
static ID3D11VertexShader*  g_vs       = nullptr;
static ID3D11PixelShader*   g_ps       = nullptr;
static ID3D11InputLayout*   g_layout   = nullptr;
static ID3D11Buffer*        g_vb       = nullptr;
static int                  g_vb_cap   = 0;
static ID3D11ShaderResourceView* g_font_srv = nullptr;
static ID3D11SamplerState*       g_font_samp = nullptr;
static ID3D11VertexShader*       g_text_vs = nullptr;
static ID3D11PixelShader*        g_text_ps = nullptr;
static ID3D11InputLayout*        g_text_layout = nullptr;
static ID3D11Buffer*             g_text_vb = nullptr;
static int                       g_text_vb_cap = 0;

struct text_vert { float x, y, u, v; uint32_t color; };

static void draw_text(text_vert*& tv, int sw, int sh, const char* str, int px, int py, uint32_t color) {
    if (!str || !*str) return;
    float cw = (2.0f * 8) / sw;
    float ch = (2.0f * 16) / sh;
    float ndc_y = 1.0f - (2.0f * py / sh);
    float ndc_x = (2.0f * px / sw) - 1.0f;
    for (int i = 0; str[i]; ++i) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 32 && c < 128) {
            int idx = c - 32, col = idx % 16, row = idx / 16;
            float u0 = col / 16.0f, v0 = row / 16.0f;
            float u1 = (col + 1) / 16.0f, v1 = (row + 1) / 16.0f;
            float x0 = ndc_x, x1 = ndc_x + cw, y0 = ndc_y, y1 = ndc_y + ch;
            tv[0] = {x0, y0, u0, v0, color}; tv[1] = {x1, y0, u1, v0, color};
            tv[2] = {x1, y1, u1, v1, color}; tv[3] = {x0, y0, u0, v0, color};
            tv[4] = {x1, y1, u1, v1, color}; tv[5] = {x0, y1, u0, v1, color};
            tv += 6;
        }
        ndc_x += cw;
    }
}

static void* g_og_fire_event_client_side = nullptr;

using get_event_name_fn      = const char*(__fastcall*)(void*);
using get_player_controller_fn = void*(__fastcall*)(void*, CUtlStringToken*);
using get_int64_fn            = int64_t(__fastcall*)(void*, const char*);

static get_event_name_fn      g_get_event_name         = nullptr;
static get_player_controller_fn g_get_player_controller = nullptr;
static get_int64_fn            g_get_int64              = nullptr;

static constexpr uintptr_t OFF_LOCAL_PAWN        = 0x23A5238;
static constexpr uintptr_t OFF_LOCAL_CONTROLLER  = 0x237FB70;
static constexpr uintptr_t OFF_ENTITY_SYS        = 0x254FE70;
static constexpr uintptr_t OFF_HEALTH            = 0x34C;
static constexpr uintptr_t OFF_TEAM              = 0x3E7;

static void* get_entity_by_index(int idx) {
    if (!g_get_base_entity || !g_entity_system) return nullptr;
    __try { return g_get_base_entity(g_entity_system, idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static int get_local_team() {
    __try {
        uintptr_t p = *(uintptr_t*)(g_client_base + OFF_LOCAL_PAWN);
        if (!p) return 0;
        return *(uint8_t*)(p + OFF_TEAM);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void* get_local_pawn() {
    __try { return *(void**)(g_client_base + OFF_LOCAL_PAWN); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* get_local_controller() {
    __try { return *(void**)(g_client_base + OFF_LOCAL_CONTROLLER); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool is_local_controller(void* controller) {
    return controller && controller == get_local_controller();
}

static const char* get_player_name(void* controller) {
    if (!controller) return "unknown";
    return (const char*)((uintptr_t)controller + 0x6F4);
}

static const char* get_team_color_hex(void* controller) {
    if (!controller) return "#c4c4c4";
    uint8_t team = 0;
    __try { team = *(uint8_t*)((uintptr_t)controller + 0x3EB); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "#c4c4c4"; }
    switch (team) {
        case 2: return "#ffd494";
        case 3: return "#85acfa";
        default: return "#c4c4c4";
    }
}

static const char* hitgroup_name(int hitgroup) {
    switch (hitgroup) {
        case 1: return "head";
        case 2: return "chest";
        case 3: return "stomach";
        case 4: return "left arm";
        case 5: return "right arm";
        case 6: return "left leg";
        case 7: return "right leg";
        default: return "body";
    }
}

static void* try_get_event_controller(void* p_game_event, CUtlStringToken* token) {
    if (!g_get_player_controller) return nullptr;
    __try { return g_get_player_controller(p_game_event, token); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool is_event_controller_local(void* p_game_event, const char* token_name) {
    if (!p_game_event || !token_name || !g_get_player_controller) return false;
    CUtlStringToken token(token_name);
    token.pad = 0xFFFFFFFF;
    void* event_controller = nullptr;
    __try { event_controller = g_get_player_controller(p_game_event, &token); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return event_controller && event_controller == get_local_controller();
}

static bool try_read_hurt_values(void* p_game_event, int64_t& dmg_health, int64_t& hitgroup, int64_t& health) {
    if (!g_get_int64) return false;
    __try {
        dmg_health = g_get_int64(p_game_event, "dmg_health");
        hitgroup = g_get_int64(p_game_event, "hitgroup");
        health = g_get_int64(p_game_event, "health");
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool hash_event_name(const char* str, uint32_t& out_hash) {
    if (!str) return false;
    __try { out_hash = fnv1a_hash(str); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out_hash = 0; return false; }
}

struct utl_buffer_t { char pad[0x80]; };
struct c_key_values_3 { char pad[0x100]; uint64_t key; void* val; char pad2[0x8]; };

using ctor_utlbuf_fn = utl_buffer_t* (__fastcall*)(utl_buffer_t*, int, int, int);
using putstring_fn  = void          (__fastcall*)(utl_buffer_t*, const char*);
using loadkv3_fn    = bool          (__fastcall*)(c_key_values_3*, void*, utl_buffer_t*, const kv3_id_t*, const char*);
using setkv3type_fn = c_key_values_3*(__fastcall*)(c_key_values_3*, unsigned int, unsigned int);
using crmat_fn      = int64_t       (__fastcall*)(void*, void*, const char*, void*, unsigned int, unsigned int);

static ctor_utlbuf_fn fn_ctor_buf = nullptr;
static putstring_fn   fn_put_str  = nullptr;
static loadkv3_fn     fn_load_kv3 = nullptr;
static setkv3type_fn  fn_set_kv3  = nullptr;
static crmat_fn       fn_crmat    = nullptr;

static bool init_mat_fns() {
    if (fn_crmat) return true;
    HMODULE t0 = GetModuleHandleA("tier0.dll");
    if (t0) {
        fn_ctor_buf = (ctor_utlbuf_fn)GetProcAddress(t0, "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z");
        fn_put_str  = (putstring_fn)GetProcAddress(t0, "?PutString@CUtlBuffer@@QEAAXPEBD@Z");
        fn_load_kv3 = (loadkv3_fn)GetProcAddress(t0, "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEAVCUtlBuffer@@AEBUKV3ID_t@@PEBDI@Z");
    }
    fn_set_kv3 = (setkv3type_fn)pattern_scan_str("client.dll",
        "40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16 00 00 00 48 8B D9 44 0F 45 C8");
    fn_crmat = (crmat_fn)pattern_scan_str("materialsystem2.dll",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 10 01 00 00 48 8B 05 ? ? ? ? 4C 8B F2");
    return fn_ctor_buf && fn_put_str && fn_load_kv3 && fn_set_kv3 && fn_crmat;
}

static c_material_2* create_mat(const char* name, const char* kv3_buf) {
    __try {
        c_key_values_3* kv = new c_key_values_3[0x10]{};
        fn_set_kv3(kv, 1U, 6U);
        int len = (int)strlen(kv3_buf);
        utl_buffer_t buf{};
        fn_ctor_buf(&buf, 0, len + 1, 1);
        fn_put_str(&buf, kv3_buf);
        kv3_id_t id{ "generic", 0x41B818518343427Eull, 0xB5F447C23C0CDF8Cull };
        fn_load_kv3(kv, nullptr, &buf, &id, nullptr);
        void* binding = nullptr;
        fn_crmat(nullptr, &binding, name, kv, 0U, 1U);
        if (!binding) return nullptr;
        return *(c_material_2**)binding;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static constexpr const char* KV3_VIS = R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_unlitgeneric.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    g_vColorTint = [0.85, 0.85, 0.85, 1.0]
    TextureAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})";

static constexpr const char* KV3_OCC = R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
    shader = "csgo_unlitgeneric.vfx"
    F_PAINT_VERTEX_COLORS = 1
    F_TRANSLUCENT = 1
    F_BLEND_MODE = 1
    F_DISABLE_Z_BUFFERING = 1
    g_vColorTint = [0.85, 0.85, 0.85, 1.0]
    TextureAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
    g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
})";

static void* __fastcall hk_generate_primitives(
    c_animatable_scene_object_desc* desc,
    c_scene_animatable_object*      object,
    void*                           a3,
    c_mesh_primitive_output_buffer*  buf)
{
    auto original = (void*(__fastcall*)(c_animatable_scene_object_desc*, c_scene_animatable_object*, void*, c_mesh_primitive_output_buffer*))
        g_og_generate_primitives;
    if (!original || !object || !buf)
        return original ? original(desc, object, a3, buf) : nullptr;

    __try {
        c_base_handle h = object->m_owner;
        if (!h.is_valid()) return original(desc, object, a3, buf);
        void* ent = get_entity_by_index(h.get_entry_index());
        if (!ent) return original(desc, object, a3, buf);

        int health = *(int*)((uintptr_t)ent + OFF_HEALTH);
        int team   = *(uint8_t*)((uintptr_t)ent + OFF_TEAM);
        int lteam  = get_local_team();
        bool enemy = false;

        if (team >= 2 && team <= 3 && team != lteam) {
            if (health > 0) {
                enemy = true;
            } else if (health >= 0) {
                enemy = true;
            }
        }

        if (!enemy) return original(desc, object, a3, buf);

        auto emit = [&](c_material_2* mat) {
            if (!mat) return;
            int prev = buf->m_arr_size;
            original(desc, object, a3, buf);
            for (int i = prev; i < buf->m_arr_size; ++i) {
                auto* p = buf->get_primitive(i);
                if (p) {
                    p->m_material = mat; p->m_material2 = mat;
                    p->m_color_r = 0xD9; p->m_color_g = 0xD9;
                    p->m_color_b = 0xD9; p->m_color_a = 0xFF;
                }
            }
        };
        if (g_mat_occluded) emit(g_mat_occluded);
        if (g_mat_visible)  emit(g_mat_visible);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return original(desc, object, a3, buf);
    }
}

static void __fastcall hk_draw_light_scene(void* dst, const void* src) {
    auto original = (void(__fastcall*)(void*, const void*))g_og_draw_light_scene;
    if (!original) return;
    __try {
        uint8_t copy[0x84];
        memcpy(copy, src, sizeof(copy));
        float* light = (float*)(copy + 0x04);
        light[0] = 0.0002f; light[1] = 0.0002f; light[2] = 0.0002f; light[3] = 0.0002f;
        original(dst, copy);
        float* d = (float*)((char*)dst + 0x04);
        d[0] = 0.0002f; d[1] = 0.0002f; d[2] = 0.0002f; d[3] = 0.0002f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { original(dst, src); }
}

static void send_chat_message(const char* message) {
    if (!message || !message[0]) return;

    using SendChatMessageFn = int64_t(__fastcall*)(void*, const char*, unsigned int, uint8_t*);
    static auto fn_send_chat = (SendChatMessageFn)0;
    if (!fn_send_chat) {
        fn_send_chat = (SendChatMessageFn)pattern_scan_str("client.dll",
            "48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 56 41 57 48 8D AC 24 ? ? ? ? B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 45 33 FF 41 8B D8");
        if (!fn_send_chat) return;
    }

    using FindHudElementFn = uintptr_t(__fastcall*)(const char*);
    static auto fn_find_hud = (FindHudElementFn)0;
    if (!fn_find_hud) {
        fn_find_hud = (FindHudElementFn)pattern_scan_str("client.dll",
            "40 53 48 83 EC ? 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24");
        if (!fn_find_hud) return;
    }

    const auto hud_ptr = fn_find_hud("CCSGO_HudVoiceStatus");
    if (!hud_ptr || hud_ptr == 32) return;

    uint8_t flags[2] = { 1, 0 };
    fn_send_chat((void*)(hud_ptr - 32), message, 0xFFFFFFFF, flags);
}

static void try_send_chat_safe(const char* message) {
    __try { send_chat_message(message); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void handle_player_hurt(void* p_game_event) {
    if (!g_get_player_controller || !g_get_int64) return;

    CUtlStringToken attacker_token("attacker");
    attacker_token.pad = 0xFFFFFFFF;
    CUtlStringToken userid_token("userid");
    userid_token.pad = 0xFFFFFFFF;

    void* attacker_controller = try_get_event_controller(p_game_event, &attacker_token);
    void* victim_controller = try_get_event_controller(p_game_event, &userid_token);

    if (!attacker_controller || !victim_controller) return;
    const bool we_attacked = is_local_controller(attacker_controller);
    if (!we_attacked && !is_local_controller(victim_controller)) return;

    if (we_attacked) {
        PlaySoundA(reinterpret_cast<LPCSTR>(hitsound::g_neverlose_wav_data), nullptr,
                   SND_MEMORY | SND_NODEFAULT | SND_ASYNC);
    }

    int64_t dmg_health = 0, hitgroup_val = 0, health = 0;
    if (!try_read_hurt_values(p_game_event, dmg_health, hitgroup_val, health)) return;
    if (dmg_health <= 0 || hitgroup_val < 0) return;

    char message[256];
    if (we_attacked) {
        const char* victim_name = get_player_name(victim_controller);
        const char* team_col = get_team_color_hex(victim_controller);
        sprintf_s(message, "<font color=\"#00FF00\"> Reflex </font><font color=\"#c4c4c4\">| hit player <font color=\"%s\">%s</font> in the %s for %lld damage (%lld hp remaining)</font>",
            team_col, victim_name, hitgroup_name((int)hitgroup_val), dmg_health, health > 0 ? health : 0);
    } else {
        const char* attacker_name = get_player_name(attacker_controller);
        const char* team_col = get_team_color_hex(attacker_controller);
        sprintf_s(message, "<font color=\"#00FF00\"> Reflex </font><font color=\"#c4c4c4\">| harmed by player <font color=\"%s\">%s</font> in the %s for %lld hp</font>",
            team_col, attacker_name, hitgroup_name((int)hitgroup_val), dmg_health);
    }
    try_send_chat_safe(message);
}

static bool __fastcall hk_fire_event_client_side(void* p_game_event_manager, void* p_game_event) {
    auto original = (bool(__fastcall*)(void*, void*))g_og_fire_event_client_side;
    if (!original) return false;

    if (!p_game_event || !g_get_event_name) return original(p_game_event_manager, p_game_event);

    const char* event_name = nullptr;
    __try { event_name = g_get_event_name(p_game_event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return original(p_game_event_manager, p_game_event); }
    if (!event_name) return original(p_game_event_manager, p_game_event);

    uint32_t event_hash = 0;
    if (!hash_event_name(event_name, event_hash))
        return original(p_game_event_manager, p_game_event);

    if (event_hash == event_hashes::player_hurt)
        handle_player_hurt(p_game_event);

    return original(p_game_event_manager, p_game_event);
}

static constexpr uint8_t FONT8x16[256][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x22,0x22,0x22,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x12,0x12,0x7F,0x24,0x24,0xFE,0x48,0x48,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x3E,0x49,0x48,0x3E,0x09,0x49,0x3E,0x08,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x60,0x92,0x94,0x68,0x08,0x16,0x29,0x49,0x06,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x38,0x44,0x44,0x38,0x30,0x4A,0x44,0x4A,0x31,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x04,0x08,0x10,0x10,0x10,0x10,0x10,0x10,0x08,0x04,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x20,0x10,0x08,0x08,0x08,0x08,0x08,0x08,0x10,0x20,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x49,0x2A,0x1C,0x08,0x1C,0x2A,0x49,0x08,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x08,0x7F,0x08,0x08,0x08,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x10,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x02,0x04,0x04,0x08,0x08,0x10,0x10,0x20,0x20,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x46,0x4A,0x52,0x62,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x18,0x28,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x02,0x04,0x08,0x10,0x20,0x40,0x7E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x04,0x08,0x1C,0x02,0x02,0x02,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x0C,0x14,0x24,0x44,0x44,0x7E,0x04,0x04,0x04,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x40,0x40,0x7C,0x02,0x02,0x02,0x44,0x38,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x1C,0x20,0x40,0x7C,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x02,0x04,0x08,0x10,0x10,0x20,0x20,0x20,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x42,0x3C,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x3E,0x02,0x04,0x38,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x00,0x00,0x00,0x00,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x00,0x00,0x00,0x00,0x08,0x08,0x10,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x10,0x08,0x04,0x02,0x04,0x08,0x10,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x02,0x04,0x08,0x08,0x08,0x00,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x5A,0x5A,0x5E,0x5A,0x5A,0x40,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x24,0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7C,0x42,0x42,0x7C,0x42,0x42,0x42,0x42,0x7C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x40,0x40,0x40,0x40,0x40,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x78,0x44,0x42,0x42,0x42,0x42,0x42,0x44,0x78,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x40,0x40,0x7E,0x40,0x40,0x40,0x40,0x7E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x40,0x40,0x7E,0x40,0x40,0x40,0x40,0x40,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x40,0x40,0x4E,0x42,0x42,0x46,0x3A,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x1F,0x04,0x04,0x04,0x04,0x04,0x44,0x44,0x38,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7C,0x42,0x42,0x42,0x7C,0x40,0x40,0x40,0x40,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x4A,0x44,0x4A,0x31,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7C,0x42,0x42,0x42,0x7C,0x48,0x44,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x42,0x40,0x38,0x04,0x02,0x02,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x18,0x18,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x42,0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x02,0x04,0x08,0x10,0x20,0x40,0x40,0x7E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x0E,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x0E,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x70,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x70,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x00},
    {0x00,0x00,0x00,0x00,0x10,0x08,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x02,0x3E,0x42,0x42,0x46,0x3A,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x40,0x40,0x5C,0x62,0x42,0x42,0x42,0x62,0x5C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x02,0x02,0x3A,0x46,0x42,0x42,0x42,0x46,0x3A,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x42,0x7E,0x40,0x40,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x0C,0x12,0x10,0x7C,0x10,0x10,0x10,0x10,0x10,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3A,0x46,0x42,0x42,0x42,0x46,0x3A,0x02,0x42,0x3C},
    {0x00,0x00,0x00,0x00,0x40,0x40,0x5C,0x62,0x42,0x42,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x00,0x18,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x04,0x00,0x0C,0x04,0x04,0x04,0x04,0x04,0x44,0x44,0x38,0x00},
    {0x00,0x00,0x00,0x00,0x40,0x40,0x44,0x48,0x50,0x70,0x48,0x44,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x76,0x49,0x49,0x49,0x49,0x49,0x49,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x5C,0x62,0x42,0x42,0x42,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x5C,0x62,0x42,0x42,0x42,0x62,0x5C,0x40,0x40,0x40},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3A,0x46,0x42,0x42,0x42,0x46,0x3A,0x02,0x02,0x02},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x40,0x40,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x40,0x38,0x04,0x02,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x10,0x10,0x7C,0x10,0x10,0x10,0x10,0x12,0x0C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x42,0x46,0x3A,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x24,0x24,0x18,0x18,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x41,0x49,0x49,0x49,0x49,0x49,0x36,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x42,0x46,0x3A,0x02,0x42,0x3C},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x04,0x08,0x10,0x20,0x40,0x7E,0x00,0x00,0x00},
};

static bool init_health_renderer(ID3D11Device* dev) {
    if (g_vs) return true;
    const char* vs_src =
        "struct VS_IN { float2 pos : POSITION; float4 col : COLOR; };\n"
        "struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR; };\n"
        "VS_OUT main(VS_IN v) {\n"
        "  VS_OUT o; o.pos = float4(v.pos.x, v.pos.y, 0, 1); o.col = v.col;\n"
        "  return o;\n"
        "}";
    const char* ps_src =
        "float4 main(float4 pos : SV_Position, float4 col : COLOR) : SV_Target { return col; }";

    ID3DBlob* vb = nullptr, *pb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(vs_src, strlen(vs_src), nullptr, nullptr, nullptr,
                          "main", "vs_4_0", 0, 0, &vb, &err))) {
        if (err) { log_fmt("VS err: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (FAILED(D3DCompile(ps_src, strlen(ps_src), nullptr, nullptr, nullptr,
                          "main", "ps_4_0", 0, 0, &pb, &err))) {
        if (err) err->Release();
        vb->Release(); return false;
    }
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    dev->CreateInputLayout(elems, 2, vb->GetBufferPointer(), vb->GetBufferSize(), &g_layout);
    vb->Release(); pb->Release();

    uint8_t pixels[128 * 256] = {};
    for (int i = 32; i < 128; ++i) {
        int row = (i - 32) / 16, col = (i - 32) % 16;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x)
                pixels[(row * 16 + y) * 128 + (col * 8 + x)] =
                    (FONT8x16[i][y] & (0x80 >> x)) ? 255 : 0;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 128; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pixels; sd.SysMemPitch = 128;
    ID3D11Texture2D* font_tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &font_tex))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = DXGI_FORMAT_R8_UNORM;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(font_tex, &svd, &g_font_srv);
        font_tex->Release();
    }
    D3D11_SAMPLER_DESC sampd{};
    sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampd.AddressU = sampd.AddressV = sampd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sampd, &g_font_samp);

    const char* tvs =
        "struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };\n"
        "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 col : COLOR; };\n"
        "VS_OUT main(VS_IN v) { VS_OUT o; o.pos = float4(v.pos.x, v.pos.y,0,1); o.uv = v.uv; o.col = v.col; return o; }";
    const char* tps =
        "Texture2D tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD, float4 col : COLOR) : SV_Target {\n"
        "  float a = tex.Sample(samp, uv).r;\n"
        "  return float4(col.rgb, col.a * a);\n"
        "}";
    if (SUCCEEDED(D3DCompile(tvs, strlen(tvs), nullptr, nullptr, nullptr,
                             "main", "vs_4_0", 0, 0, &vb, &err))) {
        dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_text_vs);
        D3D11_INPUT_ELEMENT_DESC te[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        dev->CreateInputLayout(te, 3, vb->GetBufferPointer(), vb->GetBufferSize(), &g_text_layout);
        vb->Release();
    }
    if (SUCCEEDED(D3DCompile(tps, strlen(tps), nullptr, nullptr, nullptr,
                             "main", "ps_4_0", 0, 0, &pb, &err))) {
        dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_text_ps);
        pb->Release();
    }

    log_write("health: renderer OK");
    return true;
}

struct esp_vert { float x, y; uint32_t color; };

static void draw_ndc_line(esp_vert*& v, float x1, float y1, float x2, float y2, uint32_t c) {
    v->x = x1; v->y = y1; v->color = c; ++v;
    v->x = x2; v->y = y2; v->color = c; ++v;
}

static void draw_filled_rect(esp_vert*& v, float l, float t, float r, float b, uint32_t c) {
    v->x = l; v->y = t; v->color = c; ++v;
    v->x = r; v->y = t; v->color = c; ++v;
    v->x = r; v->y = b; v->color = c; ++v;
    v->x = l; v->y = t; v->color = c; ++v;
    v->x = r; v->y = b; v->color = c; ++v;
    v->x = l; v->y = b; v->color = c; ++v;
}

static HRESULT STDMETHODCALLTYPE hk_present_v2(IDXGISwapChain* swap, UINT sync, UINT flags) {
    auto original = (HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT))g_og_present;
    if (!original) return E_FAIL;

    if (!g_d3d_dev) {
        if (swap->GetDevice(__uuidof(ID3D11Device), (void**)&g_d3d_dev) == S_OK) {
            g_d3d_dev->GetImmediateContext(&g_d3d_ctx);
            init_health_renderer(g_d3d_dev);
        }
    }
    if (!g_d3d_ctx || !g_vs) return original(swap, sync, flags);

    DXGI_SWAP_CHAIN_DESC scd;
    if (swap->GetDesc(&scd) != S_OK) return original(swap, sync, flags);
    int sw = scd.BufferDesc.Width, sh = scd.BufferDesc.Height;

    void* local = get_local_pawn();
    int lteam = get_local_team();
    if (!local || !lteam) return original(swap, sync, flags);

    __try {
        static esp_vert pool[20000];
        static esp_vert tri_pool[20000];
        esp_vert* v = pool;
        esp_vert* tri = tri_pool;

        static text_vert text_pool[4096];
        text_vert* tv = text_pool;

        const char* wm = "Reflex";
        draw_text(tv, sw, sh, wm, sw - (int)strlen(wm) * 8 - 10, 10, 0xFF00FF00);

        int line_count = (int)(v - pool);
        int tri_count = (int)(tri - tri_pool);
        int text_count = (int)(tv - text_pool);
        if (line_count == 0 && tri_count == 0 && text_count == 0) return original(swap, sync, flags);

        int line_needed = line_count * (int)sizeof(esp_vert);
        if (g_vb && line_count > g_vb_cap) { g_vb->Release(); g_vb = nullptr; }
        if (!g_vb && line_count > 0) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = max(line_needed, 8192);
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_d3d_dev->CreateBuffer(&bd, nullptr, &g_vb);
            g_vb_cap = bd.ByteWidth / (int)sizeof(esp_vert);
        }

        static ID3D11Buffer* g_tri_vb = nullptr;
        static int g_tri_vb_cap = 0;
        int tri_needed = tri_count * (int)sizeof(esp_vert);
        if (g_tri_vb && tri_count > g_tri_vb_cap) { g_tri_vb->Release(); g_tri_vb = nullptr; }
        if (!g_tri_vb && tri_count > 0) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = max(tri_needed, 8192);
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_d3d_dev->CreateBuffer(&bd, nullptr, &g_tri_vb);
            g_tri_vb_cap = bd.ByteWidth / (int)sizeof(esp_vert);
        }
        if (!g_vb && line_count > 0) return original(swap, sync, flags);

        int text_needed = text_count * (int)sizeof(text_vert);
        if (g_text_vb && text_count > g_text_vb_cap) { g_text_vb->Release(); g_text_vb = nullptr; }
        if (!g_text_vb && text_count > 0) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = max(text_needed, 4096);
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_d3d_dev->CreateBuffer(&bd, nullptr, &g_text_vb);
            g_text_vb_cap = bd.ByteWidth / (int)sizeof(text_vert);
        }

        ID3D11Texture2D* bb = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        if (swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb) != S_OK || !bb)
            return original(swap, sync, flags);
        g_d3d_dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();

        if (!rtv) return original(swap, sync, flags);

        g_d3d_ctx->OMSetRenderTargets(1, &rtv, nullptr);

        ID3D11BlendState* blend_save = nullptr;
        FLOAT blend_factor_save[4];
        UINT sample_mask_save;
        g_d3d_ctx->OMGetBlendState(&blend_save, blend_factor_save, &sample_mask_save);

        ID3D11DepthStencilState* ds_save = nullptr;
        UINT stencil_ref_save;
        g_d3d_ctx->OMGetDepthStencilState(&ds_save, &stencil_ref_save);

        ID3D11RasterizerState* rs_save = nullptr;
        g_d3d_ctx->RSGetState(&rs_save);

        static ID3D11BlendState* g_blend = nullptr;
        if (!g_blend) {
            D3D11_BLEND_DESC bd{};
            bd.AlphaToCoverageEnable = FALSE;
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            g_d3d_dev->CreateBlendState(&bd, &g_blend);
        }

        static ID3D11RasterizerState* g_raster = nullptr;
        if (!g_raster) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.ScissorEnable = FALSE;
            rd.DepthClipEnable = TRUE;
            g_d3d_dev->CreateRasterizerState(&rd, &g_raster);
        }

        static ID3D11DepthStencilState* g_ds_overlay = nullptr;
        if (!g_ds_overlay) {
            D3D11_DEPTH_STENCIL_DESC dsd{};
            dsd.DepthEnable = FALSE;
            dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dsd.StencilEnable = FALSE;
            g_d3d_dev->CreateDepthStencilState(&dsd, &g_ds_overlay);
        }

        g_d3d_ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
        g_d3d_ctx->OMSetDepthStencilState(g_ds_overlay, 0);
        g_d3d_ctx->RSSetState(g_raster);

        D3D11_VIEWPORT vp{ 0, 0, (float)sw, (float)sh, 0, 1 };
        g_d3d_ctx->RSSetViewports(1, &vp);

        if (g_vb && line_count > 0) {
            D3D11_MAPPED_SUBRESOURCE map;
            if (g_d3d_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map) == S_OK) {
                memcpy(map.pData, pool, line_count * sizeof(esp_vert));
                g_d3d_ctx->Unmap(g_vb, 0);
            }
            UINT stride = sizeof(esp_vert), off = 0;
            g_d3d_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
            g_d3d_ctx->IASetInputLayout(g_layout);
            g_d3d_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            g_d3d_ctx->VSSetShader(g_vs, nullptr, 0);
            g_d3d_ctx->PSSetShader(g_ps, nullptr, 0);
            g_d3d_ctx->Draw(line_count, 0);
        }

        if (g_tri_vb && tri_count > 0) {
            D3D11_MAPPED_SUBRESOURCE map;
            if (g_d3d_ctx->Map(g_tri_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map) == S_OK) {
                memcpy(map.pData, tri_pool, tri_count * sizeof(esp_vert));
                g_d3d_ctx->Unmap(g_tri_vb, 0);
            }
            UINT stride = sizeof(esp_vert), off = 0;
            g_d3d_ctx->IASetVertexBuffers(0, 1, &g_tri_vb, &stride, &off);
            g_d3d_ctx->IASetInputLayout(g_layout);
            g_d3d_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_d3d_ctx->VSSetShader(g_vs, nullptr, 0);
            g_d3d_ctx->PSSetShader(g_ps, nullptr, 0);
            g_d3d_ctx->Draw(tri_count, 0);
        }

        if (g_text_vb && text_count > 0 && g_text_vs && g_text_ps && g_text_layout && g_font_srv && g_font_samp) {
            D3D11_MAPPED_SUBRESOURCE map;
            if (g_d3d_ctx->Map(g_text_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map) == S_OK) {
                memcpy(map.pData, text_pool, text_count * sizeof(text_vert));
                g_d3d_ctx->Unmap(g_text_vb, 0);
            }
            UINT stride = sizeof(text_vert), off = 0;
            g_d3d_ctx->IASetVertexBuffers(0, 1, &g_text_vb, &stride, &off);
            g_d3d_ctx->IASetInputLayout(g_text_layout);
            g_d3d_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_d3d_ctx->VSSetShader(g_text_vs, nullptr, 0);
            g_d3d_ctx->PSSetShader(g_text_ps, nullptr, 0);
            g_d3d_ctx->PSSetShaderResources(0, 1, &g_font_srv);
            g_d3d_ctx->PSSetSamplers(0, 1, &g_font_samp);
            g_d3d_ctx->Draw(text_count, 0);
        }

        g_d3d_ctx->OMSetBlendState(blend_save, blend_factor_save, sample_mask_save);
        if (blend_save) blend_save->Release();
        g_d3d_ctx->OMSetDepthStencilState(ds_save, stencil_ref_save);
        if (ds_save) ds_save->Release();
        g_d3d_ctx->RSSetState(rs_save);
        if (rs_save) rs_save->Release();
        rtv->Release();

    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return original(swap, sync, flags);
}

static bool hook_present() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetForegroundWindow();
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* ts = nullptr;
    ID3D11Device* td = nullptr;
    ID3D11DeviceContext* tc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &ts, &td, &fl, &tc)))
        return false;
    void* vt = *(void**)ts;
    MH_CreateHook(((void**)vt)[8], hk_present_v2, &g_og_present);
    MH_EnableHook(((void**)vt)[8]);
    ts->Release(); td->Release(); tc->Release();
    log_write("hook_present: OK");
    return true;
}

static bool init_hooks() {
    log_write("hook: MH_Init...");
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    g_client_base = (uintptr_t)GetModuleHandleA("client.dll");
    if (!g_client_base) return false;
    g_entity_system = *(void**)(g_client_base + OFF_ENTITY_SYS);
    g_get_base_entity = (get_entity_fn)pattern_scan_str("client.dll", "4C 8D 49 ? 81 FA");
    log_fmt("hook: get_base_entity=%p", g_get_base_entity);
    if (!g_get_base_entity) return false;

    HMODULE hss = GetModuleHandleA("scenesystem.dll");
    if (hss) {
        using cif_t = void*(__stdcall*)(const char*, int*);
        auto cif = (cif_t)GetProcAddress(hss, "CreateInterface");
        if (cif) {
            void* ss = cif("SceneSystem_002", nullptr);
            if (!ss) ss = cif("SceneSystem_001", nullptr);
            if (ss) {
                auto vt = *(void***)ss;
                auto get_desc = (void*(__fastcall*)(void*, const char*))vt[17];
                if (get_desc && (uintptr_t)get_desc > 0x10000) {
                    void* ad = nullptr;
                    __try { ad = get_desc(ss, "AnimatableSceneObjectDesc"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    if (ad) {
                        void** avt = *(void***)ad;
                        if (avt && avt[4] && (uintptr_t)avt[4] > 0x10000 &&
                            MH_CreateHook(avt[4], hk_generate_primitives, &g_og_generate_primitives) == MH_OK &&
                            MH_EnableHook(avt[4]) == MH_OK)
                            log_write("hook: GP OK");
                    }
                }
            }
        }
    }

    uintptr_t dl = pattern_scan_str("scenesystem.dll",
        "8B 02 89 01 F2 0F 10 42 04 F2 0F 11 41 04 8B 42 0C 89 41 0C F2 0F 10 42");
    if (dl) { MH_CreateHook((void*)dl, hk_draw_light_scene, &g_og_draw_light_scene); MH_EnableHook((void*)dl); }

    g_get_event_name = (get_event_name_fn)pattern_scan_str("client.dll",
        "8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3");
    g_get_player_controller = (get_player_controller_fn)pattern_scan_str("client.dll",
        "48 83 EC 38 8B 02 4C 8D 44 24 20");
    g_get_int64 = (get_int64_fn)pattern_scan_str("client.dll",
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 01 41 8B F0");
    log_fmt("hook: event_get_name=%p get_ctrl=%p get_i64=%p", g_get_event_name, g_get_player_controller, g_get_int64);

    uintptr_t fire_event_addr = pattern_scan_str("client.dll",
        "40 53 41 54 41 56 48 83 EC ? 4C 8B F2");
    if (fire_event_addr) {
        if (MH_CreateHook((void*)fire_event_addr, hk_fire_event_client_side, &g_og_fire_event_client_side) == MH_OK &&
            MH_EnableHook((void*)fire_event_addr) == MH_OK)
            log_write("hook: FireEventClientSide OK");
        else
            log_fmt("hook: FireEventClientSide FAIL (addr=%p)", (void*)fire_event_addr);
    } else {
        log_write("hook: FireEventClientSide NOT FOUND");
    }

    hook_present();
    log_fmt("hook: done GP=%p DL=%p", g_og_generate_primitives, g_og_draw_light_scene);
    return g_og_generate_primitives != nullptr;
}

static bool ensure_materials() {
    if (g_mat_visible) return true;
    if (!init_mat_fns()) return false;
    g_mat_visible  = create_mat("materials/cheat/chams_gray_visible.vmat", KV3_VIS);
    g_mat_occluded = create_mat("materials/cheat/chams_gray_occluded.vmat", KV3_OCC);
    return g_mat_visible != nullptr;
}

static DWORD WINAPI init_thread(LPVOID) {
    log_init();
    log_write("=== start ===");
    while (!GetModuleHandleA("client.dll") || !GetModuleHandleA("scenesystem.dll") ||
           !GetModuleHandleA("materialsystem2.dll") || !GetModuleHandleA("tier0.dll"))
        Sleep(100);
    log_write("modules ready, sleep 2s...");
    Sleep(2000);
    __try { if (init_hooks()) log_write("hooks: OK"); else log_write("hooks: FAILED"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_write("hooks: CRASHED"); }
    for (int i = 0; i < 600; ++i) { if (ensure_materials()) break; Sleep(100); }
    log_fmt("materials: %d", g_mat_visible != nullptr);
    while (!(GetAsyncKeyState(VK_END) & 1)) Sleep(200);
    log_write("unloading...");
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(100);
    MH_Uninitialize();
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    ExitThread(0);
}

static void kick() {
    HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self_module = hmod; kick(); }
    return TRUE;
}

extern "C" __declspec(dllexport) void Initialize() { kick(); }
