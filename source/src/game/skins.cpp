// A choice of character for each player: Torque, or Blackmore (asked for on 2026-11-27: "an option for Player 1 and/or
// Player 2 to use Blackmore's character model instead of Torque").
//
// What Blackmore is in the game's files. He is made on Torque's own body: the same skinned mesh of 4,624 triangles
// and the same skeleton, with his own textures (a bald, scarred head with his hands and eyes on one texture, a red
// pinstripe suit, dress shoes). He is only in the last level's package (09B_Slums_Grouphome_level, package 114):
// mesh 0x4D, its three materials (0x2B1: the head, with the Player package's own blood layer 0x3C0247; 0x2B4 and
// 0x2B5: the suit) and three textures (0x48 head, 0x49 trousers and shoes, 0x4A jacket). Mesh 0x168 of that level
// is the same body again with a glow material (a vision of him), not used here.
//
// How a package is kept (SRSC, as the PS2 study read it, work\trace\tools\lvl.py): "SRSC", a 16-bit version, the
// directory's place and its count; the directory has 14-byte entries {u16 kind, u32 id, u32 offset, u32 size},
// sorted by the id's low half (the index). Files: .mdu (meshes: kinds 0x200 head, 0x203, 0x204 triangles {u16 3,
// u16 material, u16 x3, u16 0}, 0x207, 0x208, 0x211, 0x216), .tdu (textures, kind 0x40: 48-byte header {width,
// height, pitch, format 4 DXT1 / 8 DXT5, ...} then the blocks), .odu (templates, kind 0x102; kind 0x103 holds one
// zlib-packed number that grows with the templates, kept roughly in step here). A reference to another resource is
// PACKAGE << 16 | INDEX. The resource manager knows a resource by that id ALONE (whatever file it is in), so the
// new indices are past the last one of all three files (with each file's own next index his jacket's texture took
// the number of one of the Player's templates).
//
// How his body is drawn. Whatever model a Player's object wears, the game draws its triangle groups with the
// Player's own materials, Torque's, place by place (the draws of his body went out with Torque's arms-and-hands,
// head and trousers textures; changing his mesh's materials, its triangles' places, the model copy's material list
// at +0xA0 or giving the Player the level's own Blackmore 0x72004D changed nothing; where the game does this was
// not found). So the textures are swapped at the draw: coop.cpp marks a Blackmore body's own draws as texture pack
// variant 2 and counts them (the 32 x 32 blood layers aside); for place k, the texture drawn (Torque's, any of its
// blood and morality stages) gets his texture k from skin_<k>.dds, copied into textures\replacement\player3 under
// that texture's key the first time it is seen (SkinImage). The runtime picks the new image up a moment later.
//
// So Blackmore is added to the Player package (60, always loaded, from English\NTSC\Common\Player): his mesh, its
// materials and its textures are copied out of the player's own disc into copies of Player.mdu, .tdu and .odu
// (Game Files\skins\<language>_<region>\), after the files' last index, with the references in the mesh head and
// the materials changed to the new indices. The game is handed those copies when it opens the Player files (local
// change in the SDK: kernel::g_replace_disc_file). Nothing of the game is shipped; the copies are made on first use
// and again whenever this code's version changes.
//
// How a player gets him. An engine object gets its model through 0x186030 (this = object; model resource, 0): the
// Player's body at its creation, the first-person arms and the body back after first person (0x26400, this =
// Player; model, old set, new set). The hook below gives a player's object Blackmore's model wherever the game
// gives it one of Torque's bodies (the Player package's meshes of 4,624 or 5,795 triangles: 1, 0xA, 0xC, 0xD, 0xE,
// 0xF); the first-person arms (0x3C0002) and the creature are left alone. A resource is asked for by id with
// 0x1A21B0 (this = [0x384B90]; id), the resource's id is at +4. Once a frame (Frame) a player whose object still has
// a Torque body while he should be Blackmore, or the other way round after a change in Options, is given the right
// one through 0x26400, as the game does when first person ends. The stand-in bodies (coop.cpp) follow their owner.
//
// Settings: p1_character and p2_character in settings.toml (0 Torque, 1 Blackmore). Options, Display and Co-op,
// Co-op settings: "Player 1 character", "Player 2 character"; Player 2's own page has his. Test switches:
// S2_P1_CHARACTER / S2_P2_CHARACTER = 0 or 1 override the settings; S2_NO_SKINS=1 leaves the Player files as they are;
// S2_SKIN_ID=<hex> gives another model id in his place; S2_P2_DRAWS=<frame> (coop.cpp) lists Player 2's draws.
// Not done: his first-person arms are Torque's (the game's own arms model; his suit has no first-person arms); the
// blood layers are laid on his body with Torque's places, as on Torque.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/disc.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

namespace fs = std::filesystem;

namespace game_coop {
int BodyOwner(uint32_t object);              // coop.cpp: 0 or 1 for a player's object or his stand-in's, else -1
uint32_t PlayerOf(int who);                  // the Player (class object) of player 0 or 1, 0 when none
uint32_t DoublePlayerOf(int who);            // the Player of that player's stand-in, 0 when none
}

namespace game_skins {
uint32_t g_p1_body = 0, g_p2_body = 0;  // settings.toml p1_character, p2_character: 0 Torque, 1 Blackmore, 2 Chief

namespace {
constexpr uint32_t kVersion = 7;  // the made files are made again when this changes
constexpr uint32_t kSetModel = 0x186030, kSwapModel = 0x26400, kResource = 0x1A21B0, kResourceManager = 0x384B90;

std::mutex g_lock;
bool g_tried = false;            // the copies have been made or found (or that failed) in this run
fs::path g_dir;                  // where they are
std::string g_made_for;          // the folder (language\region) they were made for
uint32_t g_blackmore = 0;        // his model's id in the Player package, 0: not there
uint32_t g_skins = 0;            // his materials, each with its texture in skin_<n>.dds
// Chief (the owner's own model, 2026-11-27: John.zip, "it can be named chief"): never shipped. Its body is made from
// the owner's FBX and textures into characters\chief beside the exe: the mesh's chunks (chief_<kind>.bin, the game's
// layout, skinned to Torque's skeleton) and one texture per place (chief_skin_<n>.dds).
uint32_t g_chief = 0, g_chief_skins = 0;
fs::path ChiefDir() { return app::ExeDir() / "characters" / "chief"; }
bool ChiefThere() {
    std::error_code ec;
    return fs::exists(ChiefDir() / "chief_200.bin", ec);
}
std::set<uint32_t> g_torque;     // Torque's bodies: model ids
thread_local bool g_reading = false;  // reading the disc's own files: no replacement

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}
uint16_t U16(const std::vector<uint8_t>& d, size_t at) {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
}
void Put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

// ---- a package file -------------------------------------------------------------------------------------------------
struct Chunk {
    uint16_t kind = 0;
    uint32_t id = 0, offset = 0, size = 0;
    uint16_t Index() const { return uint16_t(id & 0xFFFF); }
};
struct Pack {
    std::vector<uint8_t> data;
    std::vector<Chunk> chunks;
    uint32_t dir = 0;
    bool Parse() {
        if (data.size() < 14 || std::memcmp(data.data(), "SRSC", 4) != 0) return false;
        dir = U32(data, 6);
        uint32_t n = U32(data, 10);
        if (uint64_t(dir) + uint64_t(n) * 14 > data.size()) return false;
        chunks.resize(n);
        for (uint32_t k = 0; k < n; k++) {
            Chunk& c = chunks[k];
            size_t at = dir + size_t(k) * 14;
            c.kind = U16(data, at);
            c.id = U32(data, at + 2);
            c.offset = U32(data, at + 6);
            c.size = U32(data, at + 10);
            if (uint64_t(c.offset) + c.size > data.size()) return false;
        }
        return true;
    }
    const Chunk* Find(uint16_t kind, uint16_t index) const {
        for (const Chunk& c : chunks)
            if (c.kind == kind && c.Index() == index) return &c;
        return nullptr;
    }
    std::vector<uint8_t> Bytes(const Chunk& c) const { return {data.begin() + c.offset, data.begin() + c.offset + c.size}; }
    uint16_t Last() const {
        uint16_t last = 0;
        for (const Chunk& c : chunks)
            if (c.kind != 0x402 && c.kind != 0x103) last = std::max(last, c.Index());
        return last;
    }
    // the file again with these chunks added (each with its id and bytes) after the last one
    std::vector<uint8_t> With(const std::vector<std::pair<Chunk, std::vector<uint8_t>>>& more,
                              const std::map<size_t, std::vector<uint8_t>>& replaced = {}) const {
        std::vector<uint8_t> out(data.begin(), data.begin() + dir);
        std::vector<Chunk> list = chunks;
        auto add = [&](const std::vector<uint8_t>& bytes) {
            while (out.size() % 16) out.push_back(0);
            uint32_t at = uint32_t(out.size());
            out.insert(out.end(), bytes.begin(), bytes.end());
            return at;
        };
        for (auto& [k, bytes] : replaced) list[k].offset = add(bytes), list[k].size = uint32_t(bytes.size());
        for (auto& [c, bytes] : more) {
            Chunk n = c;
            n.offset = add(bytes);
            n.size = uint32_t(bytes.size());
            list.push_back(n);
        }
        while (out.size() % 4) out.push_back(0);
        uint32_t new_dir = uint32_t(out.size());
        for (const Chunk& c : list) {
            uint8_t e[14];
            std::memcpy(e, &c.kind, 2);
            std::memcpy(e + 2, &c.id, 4);
            std::memcpy(e + 6, &c.offset, 4);
            std::memcpy(e + 10, &c.size, 4);
            out.insert(out.end(), e, e + 14);
        }
        size_t tail = dir + chunks.size() * 14;
        out.insert(out.end(), data.begin() + tail, data.end());
        Put32(out, 6, new_dir);
        Put32(out, 10, uint32_t(list.size()));
        return out;
    }
};

bool ReadDisc(const std::vector<std::string>& parts, std::vector<uint8_t>& out) {
    std::string path = "\\Device\\CdRom0";
    for (const std::string& p : parts) path += "\\" + p;
    g_reading = true;
    bool ok = kernel::ReadGameFile(path, out);
    g_reading = false;
    return ok;
}
int PackageId(const std::vector<std::string>& db_parts) {
    std::vector<uint8_t> d;
    if (!ReadDisc(db_parts, d)) return -1;
    std::string text(d.begin(), d.end());
    size_t at = text.find("\nid ");
    return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + 4);
}

// a template's slots (work\trace\tools\lvl.py): where they start, how many, and its class; false for a packed one
bool Slots(const std::vector<uint8_t>& t, size_t* first, uint32_t* count, uint32_t* cls) {
    if (t.size() < 32 || (t[0] == 0x78 && t[1] == 0x9C)) return false;
    size_t base = 14 + (U16(t, 12) == 1 ? 64 : 0);
    if (base + 14 > t.size()) return false;
    *cls = U32(t, base);
    *first = base + 14;
    *count = std::min<uint32_t>(U16(t, base + 10), uint32_t((t.size() - *first) / 4));
    return true;
}
uint32_t Triangles(const Pack& mdu, uint16_t index) {
    const Chunk* c = mdu.Find(0x204, index);
    return c && c->size >= 4 ? U32(mdu.data, c->offset) : 0;
}

// A body's skeleton and skin (kind 0x208): 64 bytes of bounds, {u32 1, u32 vertices, u32 bones}, then per bone
// {u32 name hash, 3 floats place, 9 floats turn (the inverse of its rest pose), u32 first child, u32 next sibling,
// u16 weights, weights x {u32 vertex, float}}, then a tail (counts, a reference, distances). Blackmore's has 58 of
// Torque's 64 bones: not the ones the game hangs carried things on (the owner, version 63: "some stuff was floating
// in the air when i changed models" and "when i had the gun equipped as blackmore, the bat was between my legs").
// So his skeleton is given Torque's 64, in Torque's order and tree: his own place, turn and weights for the bones he
// has (found by name), Torque's for the others, with no weights. Empty when a skeleton is not laid out as expected.
std::vector<uint8_t> FullSkeleton(const std::vector<uint8_t>& his, const std::vector<uint8_t>& torque) {
    struct Bone {
        size_t at = 0, size = 0;  // the record in its chunk, weights included
        uint32_t hash = 0;
    };
    auto read = [](const std::vector<uint8_t>& b, std::vector<Bone>& bones) -> size_t {
        if (b.size() < 76 || U32(b, 64) != 1) return 0;
        uint32_t count = U32(b, 72);
        size_t p = 76;
        for (uint32_t k = 0; k < count && k < 256; k++) {
            if (p + 62 > b.size()) return 0;
            Bone bone;
            bone.at = p, bone.hash = U32(b, p);
            size_t n = U16(b, p + 60);
            bone.size = 62 + 8 * n;
            if (p + bone.size > b.size()) return 0;
            p += bone.size;
            bones.push_back(bone);
        }
        return bones.size() == count ? p : 0;
    };
    std::vector<Bone> mine, full;
    size_t my_end = read(his, mine), full_end = read(torque, full);
    if (!my_end || !full_end) return {};
    std::map<uint32_t, const Bone*> by_hash;
    for (const Bone& b : mine) by_hash[b.hash] = &b;
    std::vector<uint8_t> out(his.begin(), his.begin() + 76);
    Put32(out, 72, uint32_t(full.size()));
    size_t kept = 0;
    for (const Bone& t : full) {
        auto it = by_hash.find(t.hash);
        if (it != by_hash.end()) {
            const Bone& m = *it->second;
            out.insert(out.end(), his.begin() + m.at, his.begin() + m.at + 52);           // name, place, turn: his
            out.insert(out.end(), torque.begin() + t.at + 52, torque.begin() + t.at + 60);  // the tree: Torque's
            out.insert(out.end(), his.begin() + m.at + 60, his.begin() + m.at + m.size);  // his weights
            kept++;
        } else {
            out.insert(out.end(), torque.begin() + t.at, torque.begin() + t.at + 60);
            out.push_back(0), out.push_back(0);
        }
    }
    if (kept != mine.size()) return {};  // a bone of his that Torque does not have
    out.insert(out.end(), his.begin() + my_end, his.end());
    return out;
}

// Makes the three copies. The parts are those of the Player file the game asked for.
bool Make(const std::vector<std::string>& asked) {
    std::vector<std::string> folder(asked.begin(), asked.end() - 3);  // ...\<language>\<region>
    auto player = [&](const char* ext) {
        std::vector<std::string> p = folder;
        p.insert(p.end(), {"Common", "Player", std::string("Player.") + ext});
        return p;
    };
    auto level = [&](const char* ext) {
        std::vector<std::string> p = folder;
        p.insert(p.end(), {"Levels", "09_Slums_Grouphome", std::string("09B_Slums_Grouphome_level.") + ext});
        return p;
    };
    int ours = PackageId(player("db")), theirs = PackageId(level("db"));
    if (ours <= 0 || theirs <= 0) {
        LOGR("skins: the Player package or the last level is not on the disc (%d, %d): Torque only", ours, theirs);
        return false;
    }
    Pack pm, pt, po, lm, lt, lo;
    struct {
        Pack* pack;
        std::vector<std::string> parts;
    } files[] = {{&pm, player("mdu")}, {&pt, player("tdu")}, {&po, player("odu")}, {&lm, level("mdu")}, {&lt, level("tdu")}, {&lo, level("odu")}};
    for (auto& f : files)
        if (!ReadDisc(f.parts, f.pack->data) || !f.pack->Parse()) {
            LOGR("skins: %s could not be read: Torque only", f.parts.back().c_str());
            return false;
        }

    // Torque's bodies: the Player package's meshes of his body's triangle count (and the 5,795 one)
    uint32_t body_triangles = Triangles(pm, 1);
    std::set<uint32_t> torque;
    for (const Chunk& c : pm.chunks)
        if (c.kind == 0x200) {
            uint32_t t = Triangles(pm, c.Index());
            if (t && (t == body_triangles || t == 5795)) torque.insert(uint32_t(ours) << 16 | c.Index());
        }

    // Blackmore: in the level, the mesh of Torque's body size whose first material is layered over the Player
    // package's blood (0x3C0247 on this disc); the other one of his size there is the glowing vision
    uint16_t mesh = 0;
    for (const Chunk& c : lm.chunks) {
        if (c.kind != 0x200 || Triangles(lm, c.Index()) != body_triangles || !lm.Find(0x208, c.Index())) continue;
        std::vector<uint8_t> head = lm.Bytes(c);
        uint32_t n = U32(head, 8);
        if (n != 3 || head.size() < 12 + 4 * n) continue;
        uint32_t first = U32(head, 12);
        if (first >> 16 != uint32_t(theirs)) continue;
        const Chunk* t = lo.Find(0x102, uint16_t(first));
        if (!t) continue;
        std::vector<uint8_t> tb = lo.Bytes(*t);
        size_t at;
        uint32_t count, cls;
        if (!Slots(tb, &at, &count, &cls) || cls != 0x03656E9F || count < 5) continue;
        if (U32(tb, at + 12) >> 16 != uint32_t(ours)) continue;
        mesh = c.Index();
        break;
    }
    if (!mesh) {
        LOGR("skins: Blackmore's mesh was not found in the last level: Torque only");
        return false;
    }

    // what is copied, and its new index in the Player package
    std::map<uint16_t, uint16_t> new_template, new_texture;
    // (new indices are past the last of all three files: one index means one thing in the whole package. With
    // each file's own next index, his jacket's texture took the number of one of the Player's templates and was
    // drawn as Torque's orange trousers)
    uint16_t next = uint16_t(std::max({po.Last(), pt.Last(), pm.Last()}) + 1);
    uint16_t new_mesh = next++;
    uint16_t& next_template = next;
    uint16_t& next_texture = next;
    std::vector<uint16_t> order;  // templates in the order met
    bool failed = false;
    std::function<void(uint32_t)> want_template;
    auto ref_of = [&](uint32_t v) { return v >> 16 == uint32_t(theirs); };
    want_template = [&](uint32_t ref) {
        if (!ref_of(ref)) return;
        uint16_t i = uint16_t(ref);
        if (new_template.count(i)) return;
        const Chunk* c = lo.Find(0x102, i);
        if (!c) {
            LOGR("skins: template %X is missing", ref);
            failed = true;
            return;
        }
        new_template[i] = next_template++;
        order.push_back(i);
        std::vector<uint8_t> t = lo.Bytes(*c);
        size_t at;
        uint32_t count, cls;
        if (!Slots(t, &at, &count, &cls)) {
            failed = true;
            return;
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = U32(t, at + 4 * k);
            if (!ref_of(v)) continue;
            if (cls == 0x03656E9F && (k == 1 || k == 3 || k == 4)) want_template(v);
            else if ((cls == 0x7E3AD512 || cls == 0xAD99E79B) && k == 1) {
                if (!lt.Find(0x40, uint16_t(v))) {
                    LOGR("skins: texture %X is missing", v);
                    failed = true;
                } else if (!new_texture.count(uint16_t(v))) new_texture[uint16_t(v)] = next_texture++;
            } else {
                LOGR("skins: template %X (class %08X) slot %u names %X, which is not understood", ref, cls, k, v);
                failed = true;
            }
        }
    };
    std::vector<uint8_t> head = lm.Bytes(*lm.Find(0x200, mesh));
    uint32_t materials = U32(head, 8);
    for (uint32_t k = 0; k < materials; k++) want_template(U32(head, 12 + 4 * k));
    if (failed) {
        LOGR("skins: Blackmore's materials could not all be followed: Torque only");
        return false;
    }
    auto ours_of = [&](uint16_t index) { return uint32_t(ours) << 16 | index; };

    // the triangles of kind 0x204: {u16 3, u16 slot, u16 x3, u16 0} each, all of them 3 (checked here)
    if (const Chunk* t = lm.Find(0x204, mesh)) {
        uint32_t n = U32(lm.data, t->offset);
        for (uint32_t k = 0; k < n; k++)
            if (U16(lm.data, t->offset + 4 + size_t(k) * 12) != 3 || U16(lm.data, t->offset + 6 + size_t(k) * 12) >= materials) {
                LOGR("skins: Blackmore's triangles are not laid out as expected (at %u): Torque only", k);
                return false;
            }
    }
    // the mesh: its chunks in the order they are in, the head's materials renumbered
    std::vector<std::pair<Chunk, std::vector<uint8_t>>> mdu_more, tdu_more, odu_more;
    for (const Chunk& c : lm.chunks) {
        if (c.Index() != mesh || c.kind < 0x200 || c.kind > 0x2FF) continue;
        std::vector<uint8_t> b = lm.Bytes(c);
        if (c.kind == 0x200)
            for (uint32_t k = 0; k < materials; k++) {
                uint32_t v = U32(b, 12 + 4 * k);
                if (ref_of(v)) Put32(b, 12 + 4 * k, ours_of(new_template[uint16_t(v)]));
            }
        if (c.kind == 0x208 && !std::getenv("S2_SKIN_OLD_SKELETON")) {  // (tests: his own 58 bones, as in version 63)
            std::vector<uint8_t> full = FullSkeleton(b, pm.Bytes(*pm.Find(0x208, 1)));
            if (full.empty()) {
                LOGR("skins: Blackmore's skeleton could not be given Torque's bones: Torque only");
                return false;
            }
            b = std::move(full);
        }
        Chunk n = c;
        n.id = (c.id & 0xFFFF0000) | new_mesh;
        mdu_more.push_back({n, std::move(b)});
    }
    uint32_t template_bytes = 0;
    for (uint16_t i : order) {
        const Chunk* c = lo.Find(0x102, i);
        std::vector<uint8_t> t = lo.Bytes(*c);
        size_t at;
        uint32_t count, cls;
        Slots(t, &at, &count, &cls);
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = U32(t, at + 4 * k);
            if (!ref_of(v)) continue;
            bool texture = (cls == 0x7E3AD512 || cls == 0xAD99E79B) && k == 1;
            Put32(t, at + 4 * k, ours_of(texture ? new_texture[uint16_t(v)] : new_template[uint16_t(v)]));
        }
        Chunk n = *c;
        n.id = (c->id & 0xFFFF0000) | new_template[i];
        template_bytes += uint32_t(t.size());
        odu_more.push_back({n, std::move(t)});
    }
    std::vector<std::pair<uint16_t, uint16_t>> textures(new_texture.begin(), new_texture.end());
    std::sort(textures.begin(), textures.end(), [](auto& a, auto& b) { return a.second < b.second; });
    for (auto& [from, to] : textures) {
        const Chunk* c = lt.Find(0x40, from);
        Chunk n = *c;
        n.id = (c->id & 0xFFFF0000) | to;
        tdu_more.push_back({n, lt.Bytes(*c)});
    }
    // Each of his materials' own texture, as a DDS file (skin_<place>.dds): what his body is drawn with (see
    // SkinImage: a Player's body is drawn with the Player's own materials, place by place, whatever model it wears)
    std::function<uint32_t(uint32_t, int)> first_texture = [&](uint32_t ref, int depth) -> uint32_t {
        const Chunk* c = ref_of(ref) && depth < 4 ? lo.Find(0x102, uint16_t(ref)) : nullptr;
        if (!c) return 0;
        std::vector<uint8_t> t = lo.Bytes(*c);
        size_t at;
        uint32_t count, cls;
        if (!Slots(t, &at, &count, &cls) || count < 2) return 0;
        if (cls == 0x7E3AD512 || cls == 0xAD99E79B) return U32(t, at + 4);
        if (cls == 0x03656E9F) return first_texture(U32(t, at + 4), depth + 1);
        return 0;
    };
    std::error_code made_ec;
    fs::create_directories(g_dir, made_ec);
    for (uint32_t k = 0; k < materials && k < 8; k++) {
        uint32_t tex = first_texture(U32(head, 12 + 4 * k), 0);
        const Chunk* c = tex ? lt.Find(0x40, uint16_t(tex)) : nullptr;
        if (!c || c->size < 48) {
            LOGR("skins: Blackmore's material %u has no texture of its own", k);
            return false;
        }
        uint32_t w = U32(lt.data, c->offset), h = U32(lt.data, c->offset + 4), format = U32(lt.data, c->offset + 12);
        uint32_t bytes = format == 4 ? w * h / 2 : w * h;
        if ((format != 4 && format != 8) || 48 + size_t(bytes) > c->size) {
            LOGR("skins: Blackmore's texture %X is of a kind not handled (format %u)", tex, format);
            return false;
        }
        // DDS: "DDS ", the 124-byte header (caps, height, width, linear size, pixel format DXT1 or DXT5), the blocks
        std::vector<uint8_t> dds(128, 0);
        auto put = [&](size_t at, uint32_t v) { std::memcpy(&dds[at], &v, 4); };
        put(0, 0x20534444), put(4, 124), put(8, 0x81007), put(12, h), put(16, w), put(20, bytes);
        put(76, 32), put(80, 4), put(84, format == 4 ? 0x31545844 : 0x35545844), put(108, 0x1000);
        dds.insert(dds.end(), lt.data.begin() + c->offset + 48, lt.data.begin() + c->offset + 48 + bytes);
        std::ofstream f(g_dir / ("skin_" + std::to_string(k) + ".dds"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(dds.data()), std::streamsize(dds.size()));
        if (!f) return false;
    }
    // the templates' number (kind 0x103: one 32-bit number, zlib-packed), grown by what is added; written as a
    // stored zlib block
    std::map<size_t, std::vector<uint8_t>> odu_replaced;
    for (size_t k = 0; k < po.chunks.size(); k++) {
        if (po.chunks[k].kind != 0x103) continue;
        std::vector<uint8_t> z = po.Bytes(po.chunks[k]);
        uint32_t old = 0;
        if (z.size() > 2) {
            char outb[16];
            int got = stbi_zlib_decode_buffer(outb, sizeof outb, reinterpret_cast<const char*>(z.data()), int(z.size()));
            if (got == 4) std::memcpy(&old, outb, 4);
        }
        if (!old) {
            LOGR("skins: the Player templates' number could not be read; left as it is");
            continue;
        }
        uint32_t now = old + template_bytes;
        std::vector<uint8_t> s = {0x78, 0x01, 0x01, 0x04, 0x00, 0xFB, 0xFF};
        uint8_t v[4];
        std::memcpy(v, &now, 4);
        s.insert(s.end(), v, v + 4);
        uint32_t a = 1, b = 0;
        for (uint8_t x : v) a = (a + x) % 65521, b = (b + a) % 65521;
        uint32_t adler = b << 16 | a;
        for (int i = 3; i >= 0; i--) s.push_back(uint8_t(adler >> (8 * i)));
        odu_replaced[k] = s;
        LOGR("skins: Player templates' number %u -> %u", old, now);
    }

    // Chief's body, when the owner's files are there: after everything else, Torque's materials in its head
    uint16_t chief = 0;
    uint32_t chief_places = 0;
    if (ChiefThere()) {
        const uint16_t kinds[] = {0x200, 0x203, 0x204, 0x208, 0x211, 0x216};
        std::vector<std::pair<Chunk, std::vector<uint8_t>>> more;
        bool whole = true;
        chief = next++;
        for (uint16_t kind : kinds) {
            char name[32];
            std::snprintf(name, sizeof name, "chief_%03x.bin", kind);
            std::ifstream f(ChiefDir() / name, std::ios::binary);
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (b.empty()) whole = false;
            Chunk c;
            c.kind = kind;
            c.id = 0x00090000u | chief;
            more.push_back({c, std::move(b)});
        }
        std::error_code ce;
        while (chief_places < 8 && fs::exists(ChiefDir() / ("chief_skin_" + std::to_string(chief_places) + ".dds"), ce)) chief_places++;
        if (whole && chief_places) {
            for (auto& m : more) mdu_more.push_back(std::move(m));
            LOGR("skins: Chief (%u places) added to the Player package as model %X", chief_places, ours_of(chief));
        } else {
            LOGR("skins: Chief's files in %s are not whole: left out", ChiefDir().string().c_str());
            chief = 0;
        }
    }
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    struct {
        const char* name;
        std::vector<uint8_t> bytes;
    } out[] = {{"Player.mdu", pm.With(mdu_more)}, {"Player.tdu", pt.With(tdu_more)}, {"Player.odu", po.With(odu_more, odu_replaced)}};
    for (auto& o : out) {
        fs::path tmp = g_dir / (std::string(o.name) + ".new");
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(o.bytes.data()), std::streamsize(o.bytes.size()));
        f.close();
        if (!f) return false;
        fs::rename(tmp, g_dir / o.name, ec);
        if (ec) return false;
    }
    std::ofstream info(g_dir / "made.txt");
    info << "version " << kVersion << "\nskins " << std::min<uint32_t>(materials, 8) << "\nblackmore " << std::hex << ours_of(new_mesh) << "\ntorque";
    for (uint32_t t : torque) info << " " << t;
    info << "\n";
    if (chief) info << "chief " << std::hex << ours_of(chief) << std::dec << " " << chief_places << "\n";
    info.close();
    LOGR("skins: Blackmore (mesh %X of package %d, %u materials, %zu textures) added to the Player package as model %X",
         mesh, theirs, unsigned(order.size()), textures.size(), ours_of(new_mesh));
    return bool(info);
}

// the made copies are still good: read what they hold
bool Known() {
    std::ifstream f(g_dir / "made.txt");
    std::string word;
    uint32_t version = 0, blackmore = 0;
    std::set<uint32_t> torque;
    while (f >> word) {
        if (word == "version") f >> version;
        else if (word == "skins") f >> g_skins;
        else if (word == "chief") f >> std::hex >> g_chief >> std::dec >> g_chief_skins;
        else if (word == "blackmore") f >> std::hex >> blackmore >> std::dec;
        else if (word == "torque") {
            std::string rest;
            std::getline(f, rest);
            for (size_t at = 0; at < rest.size();) {
                char* end = nullptr;
                unsigned long v = std::strtoul(rest.c_str() + at, &end, 16);
                if (end == rest.c_str() + at) break;
                torque.insert(uint32_t(v));
                at = size_t(end - rest.c_str());
            }
        }
    }
    std::error_code ec;
    if (version != kVersion || !blackmore || torque.empty() || !g_skins) return false;
    if (ChiefThere() != (g_chief != 0)) return false;  // Chief's files came or went: made again
    for (uint32_t k = 0; k < g_skins; k++)
        if (!fs::exists(g_dir / ("skin_" + std::to_string(k) + ".dds"), ec)) return false;
    for (const char* n : {"Player.mdu", "Player.tdu", "Player.odu"})
        if (!fs::exists(g_dir / n, ec)) return false;
    g_blackmore = blackmore;
    g_torque = torque;
    return true;
}

fs::path Replace(const std::vector<std::string>& parts) {
    if (g_reading || parts.size() < 5) return {};
    static const bool off = std::getenv("S2_NO_SKINS") != nullptr;
    if (off) return {};
    size_t n = parts.size();
    if (Upper(parts[n - 3]) != "COMMON" || Upper(parts[n - 2]) != "PLAYER") return {};
    std::string name = Upper(parts[n - 1]);
    if (name != "PLAYER.MDU" && name != "PLAYER.TDU" && name != "PLAYER.ODU") return {};
    std::lock_guard<std::mutex> lock(g_lock);
    std::string folder;
    for (size_t k = 0; k + 3 < n; k++) folder += (folder.empty() ? "" : "_") + parts[k];
    if (!g_tried || folder != g_made_for) {
        g_tried = true;
        g_made_for = folder;
        g_blackmore = 0, g_chief = 0, g_chief_skins = 0;
        g_torque.clear();
        g_dir = app::ExeDir() / "skins" / folder;
        if (!Known()) {
            if (Make(parts)) Known();
        } else {
            LOGR("skins: Player package with Blackmore (model %X) from %s", g_blackmore, g_dir.string().c_str());
        }
    }
    if (!g_blackmore) return {};
    return g_dir / (name == "PLAYER.MDU" ? "Player.mdu" : name == "PLAYER.TDU" ? "Player.tdu" : "Player.odu");
}

// ---- the players' models --------------------------------------------------------------------------------------------
uint32_t Want(int who) {
    static const char* env[2] = {std::getenv("S2_P1_CHARACTER"), std::getenv("S2_P2_CHARACTER")};
    uint32_t c = env[who] ? uint32_t(std::atoi(env[who])) : who ? g_p2_body : g_p1_body;
    return (c == 2 && !g_chief) || c > 2 ? 0 : c;
}
uint32_t g_blackmore_resource = 0;
std::map<uint32_t, uint32_t> g_torque_resource;  // Torque's bodies asked for: id -> resource
uint32_t g_last_torque = 0;                       // the Torque body last taken off a player (put back on a change)

uint32_t ModelId(uint32_t resource);
uint32_t Resource(X86Context& ctx, uint32_t id) {
    uint32_t ecx = ctx.ecx, eax = ctx.eax;
    ctx.ecx = kernel::R32(kResourceManager);
    uint32_t r = kernel::CallGuest(ctx, kResource, {id});
    ctx.ecx = ecx, ctx.eax = eax;
    return r && mem::IsCommitted(r + 0x10) && kernel::R32(r + 4) == id ? r : 0;
}
uint32_t Blackmore(X86Context& ctx) {
    if (!g_blackmore) return 0;
    static const char* other = std::getenv("S2_SKIN_ID");  // tests: another model id in his place (hex)
    if (other) {
        static uint32_t r = 0;
        uint32_t id = uint32_t(std::strtoul(other, nullptr, 16));
        if (!r || ModelId(r) != id) r = Resource(ctx, id);
        return r;
    }
    if (g_blackmore_resource && mem::IsCommitted(g_blackmore_resource + 0x10) && kernel::R32(g_blackmore_resource + 4) == g_blackmore)
        return g_blackmore_resource;
    g_blackmore_resource = Resource(ctx, g_blackmore);
    if (g_blackmore_resource) LOGR("skins: Blackmore's model %X is resource %08X", g_blackmore, g_blackmore_resource);
    else LOGR("skins: Blackmore's model %X was asked for and not given", g_blackmore);
    return g_blackmore_resource;
}
uint32_t CharacterOf(uint32_t id) { return !id ? 0 : id == g_blackmore ? 1 : id == g_chief ? 2 : 0; }
uint32_t g_chief_resource = 0;
uint32_t Wear(X86Context& ctx, uint32_t character) {  // the model resource of that character
    if (character == 1) return Blackmore(ctx);
    if (character != 2 || !g_chief) return 0;
    if (!g_chief_resource || ModelId(g_chief_resource) != g_chief) {
        g_chief_resource = Resource(ctx, g_chief);
        LOGR("skins: Chief's model %X is resource %08X", g_chief, g_chief_resource);
    }
    return g_chief_resource;
}
const char* kNames[3] = {"Torque", "Blackmore", "Chief"};
uint32_t Torque(X86Context& ctx, uint32_t id) {
    uint32_t& r = g_torque_resource[id];
    if (!r || !mem::IsCommitted(r + 0x10) || kernel::R32(r + 4) != id) r = Resource(ctx, id);
    return r;
}
uint32_t ModelId(uint32_t resource) { return resource && mem::IsCommitted(resource + 0x10) ? kernel::R32(resource + 4) : 0; }

}  // namespace

void Install() { kernel::g_replace_disc_file = Replace; }

// once a frame: each player (and his stand-in) wears what he should, unless he is in first person (his body comes
// back through the hook when it ends) or in another form
void Frame(X86Context& ctx) {
    if (!g_blackmore) return;
    for (int who = 0; who < 2; who++) {
        for (uint32_t player : {game_coop::PlayerOf(who), game_coop::DoublePlayerOf(who)}) {
            if (!player || !mem::IsCommitted(player + 0x770)) continue;
            uint32_t object = kernel::R32(player + 4);
            if (!object || !mem::IsCommitted(object + 0x110) || int32_t(kernel::R32(object + 8)) < 0) continue;
            uint32_t now = kernel::R32(object + 0x64);
            bool first_person = (kernel::R32(player + 0xBC) & 0x8000) != 0;
            uint32_t want = Want(who);
            uint32_t give = 0, wears = CharacterOf(now);
            if (want && (g_torque.count(now) || (wears && wears != want))) give = Wear(ctx, want);
            else if (!want && wears) give = Torque(ctx, g_last_torque ? g_last_torque : *g_torque.begin());
            if (first_person) {  // the body he gets back when first person ends
                uint32_t kept = kernel::R32(player + 0x764), kept_id = ModelId(kept), kept_wears = CharacterOf(kept_id);
                if (want && (g_torque.count(kept_id) || (kept_wears && kept_wears != want))) {
                    if (uint32_t b = Wear(ctx, want)) kernel::W32(player + 0x764, b);
                } else if (!want && kept_wears) {
                    if (uint32_t t = Torque(ctx, g_last_torque ? g_last_torque : *g_torque.begin())) kernel::W32(player + 0x764, t);
                }
                continue;
            }
            if (!give) continue;
            uint32_t set = kernel::R32(player + 0x76C);
            uint32_t ecx = ctx.ecx, eax = ctx.eax;
            ctx.ecx = player;
            kernel::CallGuest(ctx, kSwapModel, {give, set, set});
            ctx.ecx = ecx, ctx.eax = eax;
            LOGR("skins: player %d%s is now %s (model %X)", who + 1, player == game_coop::PlayerOf(who) ? "" : "'s stand-in",
                 kNames[want], kernel::R32(object + 0x64));
        }
    }
}

bool Available() { return g_blackmore != 0; }
bool ChiefAvailable() { return g_chief != 0; }

// Is this engine object a player's body (or his stand-in) wearing Blackmore? 1 then, else 0.
// (2 when he wears Chief)
int BodySkin(uint32_t object) {
    if (!g_blackmore || !object || !mem::IsCommitted(object + 0x110)) return 0;
    uint32_t c = CharacterOf(kernel::R32(object + 0x64));
    return c && game_coop::BodyOwner(object) >= 0 ? int(c) : 0;
}

// The texture pack image for the draw of place k of a Blackmore body, whose texture (Torque's for that place) has
// this key: his own texture for that place, in textures\replacement\player3 (variant 2), made once (a copy of
// skin_<k>.dds). Why: whatever model a Player wears, the game draws its triangle groups with the Player's own
// materials, Torque's, place by place (seen in the draws: his three groups came out in Torque's arms and hands,
// head and trousers textures; changing the model's head, its triangles' places or the copy's material list
// changed nothing). The runtime swaps the image by key for the draws marked as his.
// (Chief's: variant 3, textures\replacement\player4, from characters\chief\chief_skin_<k>.dds)
void SkinImage(int character, uint64_t key, uint32_t k, uint32_t w, uint32_t h) {
    if (character < 1 || character > 2 || k >= (character == 1 ? g_skins : g_chief_skins) || !key) return;
    static std::set<uint64_t> done[3];
    if (!done[character].insert(key).second) return;
    fs::path dir = app::ExeDir() / "textures" / "replacement" / (character == 1 ? "player3" : "player4");
    char name[80];
    std::snprintf(name, sizeof name, "%ux%u_%016llx_%s%u.dds", w, h, (unsigned long long)key, character == 1 ? "blackmore" : "chief", k);
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return;
    fs::create_directories(dir, ec);
    fs::path from = character == 1 ? g_dir / ("skin_" + std::to_string(k) + ".dds") : ChiefDir() / ("chief_skin_" + std::to_string(k) + ".dds");
    fs::copy_file(from, dir / name, fs::copy_options::overwrite_existing, ec);
    LOGR("skins: %s's place %u is drawn over Torque's texture %ux%u_%016llx (%s)", kNames[character], k, w, h, (unsigned long long)key, ec ? "could not be written" : "written");
}
}  // namespace game_skins

// 0x186030 (this = engine object; model resource, 0): an object is given a model. A player's object (or his
// stand-in's) given one of Torque's bodies gets Blackmore's when that player is Blackmore.
X86_EXTERN_FUNC(__imp__sub_00186030);
X86_FUNC(sub_00186030) {
    using namespace game_skins;
    uint32_t object = ctx.ecx, resource = kernel::R32(ctx.esp + 4);
    if (g_blackmore && resource) {
        uint32_t id = ModelId(resource);
        int who = g_torque.count(id) ? game_coop::BodyOwner(object) : -1;
        if (who >= 0) {
            g_last_torque = id;
            if (uint32_t c = Want(who)) {
                if (uint32_t b = Wear(ctx, c)) kernel::W32(ctx.esp + 4, b);
            }
        }
    }
    __imp__sub_00186030(ctx, base);
}
