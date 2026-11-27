// Chief: the owner's own model as a third character (asked for on 2026-11-27: "i want this model in too as an option",
// "it can be named chief"). Nothing of it is in the program or the repository: the player puts the model's zip
// (John.zip: John.FBX and its textures Helmet, Torso, Arms, Legs .png) in Game Files\characters, and the game makes
// the body from it the first time (characters\chief: chief_<kind>.bin, the body's chunks in the game's layout, and
// chief_skin_<n>.dds, one texture per place), which skins.cpp then adds to the Player package like Blackmore.
//
// The model has no skeleton (a static mesh in eight parts, 25,393 triangles, standing with its arms forward), so it is
// fitted to Torque's body:
//   - the parts are put together (each FBX model's PreRotation, rotation, scale and place), its arms are turned about
//     their shoulders to point out as Torque's do (12 degrees below level) and made as long as his (0.60 m from
//     shoulder to finger tips), the whole is scaled to his height, centred and turned to face his way;
//   - it is made lighter (half-edge collapses by quadric error, endpoints only so each vertex keeps its own uv; the
//     edges of the uv islands stay): the game's own largest body has 5,762 vertices, and the full model crashed it;
//   - each vertex takes the bone weights of the four nearest of Torque's skinned vertices (by inverse distance; the
//     three largest kept, as the game skins with three at most);
//   - the body is written as Torque's is laid out: positions and normals (0x203), triangles with their places and
//     corner uvs (0x204), Torque's 64-bone skeleton with the new weights (0x208), seam copies, uvs and triangle strips
//     per place (0x211: real strips; one index list per triangle overran the game's room for them), the vertices'
//     numbers (0x216). The places are drawn with Torque's materials and get the model's own textures at the draw
//     (skins.cpp SkinImage, variant 3).
// The work was first done in Python (the scratchpad's chiefconv.py, decimate.py) and checked in the game; this is the
// same in C++, run on the player's PC. xbox\NOTES.md, "Character choice", has the layout of each chunk.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

namespace fs = std::filesystem;

namespace game_chief {
namespace {
using Bytes = std::vector<uint8_t>;
template <class T> T Get(const Bytes& d, size_t at) {
    T v{};
    if (at + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + at, sizeof(T));
    return v;
}
template <class T> void Add(Bytes& out, T v) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}
std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- the zip ---------------------------------------------------------------------------------------------------------
bool ReadZip(const fs::path& path, std::map<std::string, Bytes>& files) {
    std::ifstream f(path, std::ios::binary);
    Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 22) return false;
    size_t end = std::string::npos;
    for (size_t at = d.size() - 22; at + 1 > 0 && d.size() - at < 70000; at--) {
        if (Get<uint32_t>(d, at) == 0x06054B50) { end = at; break; }
        if (at == 0) break;
    }
    if (end == std::string::npos) return false;
    uint16_t count = Get<uint16_t>(d, end + 10);
    size_t p = Get<uint32_t>(d, end + 16);
    for (uint16_t k = 0; k < count; k++) {
        if (Get<uint32_t>(d, p) != 0x02014B50) return false;
        uint16_t method = Get<uint16_t>(d, p + 10);
        uint32_t packed = Get<uint32_t>(d, p + 20), size = Get<uint32_t>(d, p + 24);
        uint16_t nl = Get<uint16_t>(d, p + 28), el = Get<uint16_t>(d, p + 30), cl = Get<uint16_t>(d, p + 32);
        uint32_t local = Get<uint32_t>(d, p + 42);
        std::string name(d.begin() + p + 46, d.begin() + p + 46 + nl);
        p += 46 + nl + el + cl;
        if (Get<uint32_t>(d, local) != 0x04034B50) return false;
        size_t data = local + 30 + Get<uint16_t>(d, local + 26) + Get<uint16_t>(d, local + 28);
        if (data + packed > d.size()) return false;
        Bytes out(size);
        if (method == 0) std::memcpy(out.data(), d.data() + data, size);
        else if (method == 8) {
            int got = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), int(size), reinterpret_cast<const char*>(d.data() + data), int(packed));
            if (got != int(size)) return false;
        } else continue;
        std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
        files[Lower(base)] = std::move(out);
    }
    return true;
}

// ---- binary FBX 7.x ----------------------------------------------------------------------------------------------------
struct Prop {
    char type = 0;
    double num = 0;
    int64_t whole = 0;
    std::string text;
    std::vector<double> reals;
    std::vector<int64_t> wholes;
};
struct Node {
    std::string name;
    std::vector<Prop> props;
    std::vector<Node> kids;
    const Node* Kid(const char* n) const {
        for (const Node& k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
};
struct Fbx {
    const Bytes& d;
    bool big = false;
    bool ok = true;
    bool Array(size_t& p, char t, Prop& out) {
        uint32_t n = Get<uint32_t>(d, p), enc = Get<uint32_t>(d, p + 4), len = Get<uint32_t>(d, p + 8);
        p += 12;
        if (p + len > d.size()) return false;
        size_t each = (t == 'd' || t == 'l') ? 8 : (t == 'f' || t == 'i') ? 4 : 1;
        Bytes raw(size_t(n) * each);
        if (enc) {
            int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(raw.data()), int(raw.size()), reinterpret_cast<const char*>(d.data() + p), int(len));
            if (got != int(raw.size())) return false;
        } else {
            if (len < raw.size()) return false;
            std::memcpy(raw.data(), d.data() + p, raw.size());
        }
        p += len;
        for (uint32_t i = 0; i < n; i++) {
            if (t == 'd') out.reals.push_back(Get<double>(raw, i * 8));
            else if (t == 'f') out.reals.push_back(Get<float>(raw, i * 4));
            else if (t == 'l') out.wholes.push_back(Get<int64_t>(raw, i * 8));
            else if (t == 'i') out.wholes.push_back(Get<int32_t>(raw, i * 4));
            else out.wholes.push_back(raw[i]);
        }
        return true;
    }
    bool Read(size_t& p, Node& node, bool& null) {
        uint64_t end, np;
        if (big) end = Get<uint64_t>(d, p), np = Get<uint64_t>(d, p + 8), p += 24;
        else end = Get<uint32_t>(d, p), np = Get<uint32_t>(d, p + 4), p += 12;
        uint8_t nl = Get<uint8_t>(d, p);
        p += 1;
        null = end == 0;
        if (null) return true;
        if (end > d.size()) return false;
        node.name.assign(d.begin() + p, d.begin() + p + nl);
        p += nl;
        for (uint64_t k = 0; k < np && k < 100000; k++) {
            Prop pr;
            pr.type = char(d[p++]);
            switch (pr.type) {
                case 'Y': pr.whole = Get<int16_t>(d, p), p += 2; break;
                case 'C': pr.whole = d[p], p += 1; break;
                case 'I': pr.whole = Get<int32_t>(d, p), p += 4; break;
                case 'L': pr.whole = Get<int64_t>(d, p), p += 8; break;
                case 'F': pr.num = Get<float>(d, p), p += 4; break;
                case 'D': pr.num = Get<double>(d, p), p += 8; break;
                case 'S': case 'R': {
                    uint32_t n = Get<uint32_t>(d, p);
                    p += 4;
                    if (p + n > d.size()) return false;
                    pr.text.assign(d.begin() + p, d.begin() + p + n);
                    p += n;
                    break;
                }
                case 'f': case 'd': case 'l': case 'i': case 'b': case 'c':
                    if (!Array(p, pr.type, pr)) return false;
                    break;
                default: return false;
            }
            if (pr.type == 'D' || pr.type == 'F') pr.whole = int64_t(pr.num);
            else if (pr.type != 'S' && pr.type != 'R' && pr.wholes.empty() && pr.reals.empty()) pr.num = double(pr.whole);
            node.props.push_back(std::move(pr));
        }
        while (p < end) {
            Node kid;
            bool kid_null;
            if (!Read(p, kid, kid_null)) return false;
            if (kid_null) break;
            node.kids.push_back(std::move(kid));
        }
        p = end;
        return true;
    }
};
bool ParseFbx(const Bytes& d, std::vector<Node>& top) {
    if (d.size() < 27 || std::memcmp(d.data(), "Kaydara FBX Binary", 18) != 0) return false;
    Fbx f{d};
    f.big = Get<uint32_t>(d, 23) >= 7500;
    size_t p = 27;
    while (p + 13 < d.size()) {
        Node n;
        bool null;
        if (!f.Read(p, n, null)) return false;
        if (null) break;
        top.push_back(std::move(n));
    }
    return true;
}
std::string NameOf(const Node& n) {
    std::string s = n.props.size() > 1 ? n.props[1].text : std::string();
    size_t z = s.find('\0');
    return z == std::string::npos ? s : s.substr(0, z);
}

// ---- small linear algebra --------------------------------------------------------------------------------------------
using V3 = std::array<double, 3>;
using M3 = std::array<std::array<double, 3>, 3>;
V3 Sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 AddV(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double Dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Apply(const M3& m, V3 v) { return {Dot(m[0], v), Dot(m[1], v), Dot(m[2], v)}; }
M3 MulM(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r[i][j] += a[i][k] * b[k][j];
    return r;
}
M3 Rot(int axis, double deg) {
    double a = deg * 3.14159265358979323846 / 180, c = std::cos(a), s = std::sin(a);
    if (axis == 0) return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
    if (axis == 1) return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
    return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}
M3 Euler(V3 r) { return MulM(Rot(2, r[2]), MulM(Rot(1, r[1]), Rot(0, r[0]))); }
M3 Identity() { return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }

// ---- the model ---------------------------------------------------------------------------------------------------------
struct Part {
    std::string name, material, texture;
    std::vector<V3> points;          // in the model's world
    std::vector<int64_t> polys;      // polygon vertex indices (the last of each polygon is ~index)
    std::vector<double> uv;          // uv pairs
    std::vector<int64_t> uv_index;   // per polygon vertex, or empty (direct)
    std::vector<double> normals;     // per polygon vertex (or per point when by_point)
    bool by_point = false;
};
bool LoadParts(const Bytes& data, std::vector<Part>& parts) {
    std::vector<Node> top;
    if (!ParseFbx(data, top)) return false;
    const Node *objects = nullptr, *connections = nullptr;
    for (const Node& n : top) {
        if (n.name == "Objects") objects = &n;
        if (n.name == "Connections") connections = &n;
    }
    if (!objects || !connections) return false;
    std::map<int64_t, const Node*> models, geometries, materials, textures;
    for (const Node& k : objects->kids) {
        if (k.props.empty()) continue;
        int64_t id = k.props[0].whole;
        if (k.name == "Model") models[id] = &k;
        else if (k.name == "Geometry") geometries[id] = &k;
        else if (k.name == "Material") materials[id] = &k;
        else if (k.name == "Texture") textures[id] = &k;
    }
    std::map<int64_t, int64_t> geometry_model, material_model, texture_material;
    for (const Node& c : connections->kids) {
        if (c.props.size() < 3 || c.props[0].text != "OO") continue;
        int64_t a = c.props[1].whole, b = c.props[2].whole;
        if (geometries.count(a) && models.count(b)) geometry_model[a] = b;
        if (materials.count(a) && models.count(b)) material_model[b] = a;
        if (textures.count(a) && materials.count(b)) texture_material[b] = a;
    }
    for (auto& [gid, mid] : geometry_model) {
        const Node& model = *models[mid];
        const Node& g = *geometries[gid];
        Part part;
        part.name = NameOf(model);
        if (material_model.count(mid)) {
            int64_t mat = material_model[mid];
            part.material = NameOf(*materials[mat]);
            if (texture_material.count(mat))
                if (const Node* rel = textures[texture_material[mat]]->Kid("RelativeFilename"))
                    if (!rel->props.empty()) part.texture = rel->props[0].text;
        }
        V3 t{0, 0, 0}, r{0, 0, 0}, s{1, 1, 1}, pre{0, 0, 0};
        if (const Node* p70 = model.Kid("Properties70"))
            for (const Node& p : p70->kids) {
                if (p.props.size() < 7) continue;
                V3 v{p.props[4].num, p.props[5].num, p.props[6].num};
                const std::string& what = p.props[0].text;
                if (what == "Lcl Translation") t = v;
                else if (what == "Lcl Rotation") r = v;
                else if (what == "Lcl Scaling") s = v;
                else if (what == "PreRotation") pre = v;
            }
        M3 m = MulM(Euler(pre), Euler(r));
        const Node* verts = g.Kid("Vertices");
        const Node* polys = g.Kid("PolygonVertexIndex");
        if (!verts || !polys || verts->props.empty() || polys->props.empty()) continue;
        const std::vector<double>& v = verts->props[0].reals;
        for (size_t i = 0; i + 2 < v.size(); i += 3) part.points.push_back(AddV(Apply(m, {v[i] * s[0], v[i + 1] * s[1], v[i + 2] * s[2]}), t));
        part.polys = polys->props[0].wholes;
        if (const Node* le = g.Kid("LayerElementUV")) {
            if (const Node* u = le->Kid("UV")) part.uv = u->props[0].reals;
            if (const Node* ui = le->Kid("UVIndex")) part.uv_index = ui->props[0].wholes;
        }
        if (const Node* le = g.Kid("LayerElementNormal")) {
            if (const Node* n = le->Kid("Normals")) part.normals = n->props[0].reals;
            if (const Node* mt = le->Kid("MappingInformationType")) part.by_point = !mt->props.empty() && mt->props[0].text.rfind("ByVert", 0) == 0;
        }
        parts.push_back(std::move(part));
    }
    return !parts.empty();
}

// ---- decimation (decimate.py) ----------------------------------------------------------------------------------------
struct Tri {
    int place, v[3];
};
void Decimate(const std::vector<V3>& P, std::vector<Tri>& tris, size_t target_tris, size_t target_verts, std::vector<int>& kept) {
    size_t n = P.size();
    std::vector<std::vector<int>> vt(n);
    std::vector<bool> alive(tris.size(), true);
    for (size_t i = 0; i < tris.size(); i++)
        for (int v : tris[i].v) vt[v].push_back(int(i));
    std::map<std::pair<int, int>, int> edges;
    for (const Tri& t : tris)
        for (int e = 0; e < 3; e++) {
            int a = t.v[e], b = t.v[(e + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}]++;
        }
    std::vector<bool> locked(n, false), removed(n, false);
    for (auto& [e, c] : edges)
        if (c == 1) locked[e.first] = locked[e.second] = true;
    std::vector<std::array<double, 16>> Q(n);
    for (auto& q : Q) q.fill(0);
    for (const Tri& t : tris) {
        V3 nrm = Cross(Sub(P[t.v[1]], P[t.v[0]]), Sub(P[t.v[2]], P[t.v[0]]));
        double L = Len(nrm);
        if (L < 1e-12) continue;
        nrm = Mul(nrm, 1 / L);
        double pl[4] = {nrm[0], nrm[1], nrm[2], -Dot(nrm, P[t.v[0]])};
        for (int v : t.v)
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) Q[v][i * 4 + j] += pl[i] * pl[j];
    }
    auto cost = [&](int u, int v) {
        double x[4] = {P[v][0], P[v][1], P[v][2], 1}, c = 0;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) c += x[i] * (Q[u][i * 4 + j] + Q[v][i * 4 + j]) * x[j];
        return c;
    };
    using Entry = std::tuple<double, int, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    auto push = [&](int u) {
        if (locked[u] || removed[u]) return;
        std::set<int> nb;
        for (int t : vt[u])
            if (alive[t])
                for (int x : tris[t].v)
                    if (x != u) nb.insert(x);
        for (int v : nb) heap.push({cost(u, v), u, v});
    };
    for (size_t u = 0; u < n; u++) push(int(u));
    size_t tri_count = tris.size(), vert_count = n;
    while (!heap.empty() && (tri_count > target_tris || vert_count > target_verts)) {
        auto [c, u, v] = heap.top();
        heap.pop();
        if (removed[u] || removed[v] || locked[u]) continue;
        double now = cost(u, v);
        if (std::fabs(c - now) > 1e-12 * std::max(1.0, std::fabs(c))) continue;
        std::vector<int> shared, moving;
        for (int t : vt[u]) {
            if (!alive[t]) continue;
            bool has_v = tris[t].v[0] == v || tris[t].v[1] == v || tris[t].v[2] == v;
            (has_v ? shared : moving).push_back(t);
        }
        if (shared.empty()) continue;
        bool ok = true;
        for (int t : moving) {
            int a = tris[t].v[0], b = tris[t].v[1], cc = tris[t].v[2];
            V3 before = Cross(Sub(P[b], P[a]), Sub(P[cc], P[a]));
            int na = a == u ? v : a, nb = b == u ? v : b, nc = cc == u ? v : cc;
            V3 after = Cross(Sub(P[nb], P[na]), Sub(P[nc], P[na]));
            double lb = Len(before), la = Len(after);
            if (la < 1e-12 || lb < 1e-12 || Dot(before, after) < 0.3 * lb * la) { ok = false; break; }
        }
        if (ok) {  // the link condition
            std::set<int> nu, nv, opp;
            for (int t : vt[u]) if (alive[t]) for (int x : tris[t].v) nu.insert(x);
            for (int t : vt[v]) if (alive[t]) for (int x : tris[t].v) nv.insert(x);
            for (int t : shared) for (int x : tris[t].v) if (x != u && x != v) opp.insert(x);
            std::set<int> common;
            for (int x : nu) if (x != u && x != v && nv.count(x)) common.insert(x);
            ok = common == opp;
        }
        if (!ok) continue;
        for (int t : shared) alive[t] = false, tri_count--;
        for (int t : moving) {
            for (int& x : tris[t].v) if (x == u) x = v;
            vt[v].push_back(t);
        }
        vt[u].clear();
        removed[u] = true, vert_count--;
        for (int i = 0; i < 16; i++) Q[v][i] += Q[u][i];
        push(v);
        std::set<int> around;
        for (int t : vt[v]) if (alive[t]) for (int x : tris[t].v) if (x != v) around.insert(x);
        for (int x : around) push(x);
    }
    std::vector<int> renum(n, -1);
    kept.clear();
    std::vector<bool> used(n, false);
    for (size_t i = 0; i < tris.size(); i++)
        if (alive[i]) for (int x : tris[i].v) used[x] = true;
    for (size_t i = 0; i < n; i++)
        if (used[i]) renum[i] = int(kept.size()), kept.push_back(int(i));
    std::vector<Tri> out;
    for (size_t i = 0; i < tris.size(); i++)
        if (alive[i]) {
            Tri t = tris[i];
            for (int& x : t.v) x = renum[x];
            out.push_back(t);
        }
    tris = std::move(out);
}

// ---- Torque's body (0x203 positions, 0x208 skeleton and weights) -----------------------------------------------------
struct Torque {
    std::vector<V3> points;                 // his skinned (base) vertices
    std::vector<std::vector<std::pair<int, float>>> weights;  // per base vertex: bone, weight
    Bytes head208;                          // bounds (64 bytes)
    std::vector<Bytes> bones;               // each bone's first 60 bytes
    Bytes tail208;
    Bytes head200;
};
bool ReadTorque(const Bytes& c200, const Bytes& c203, const Bytes& c208, Torque& t) {
    uint16_t n = Get<uint16_t>(c203, 0);
    if (c203.size() < 2 + 16u * n || c208.size() < 76) return false;
    std::vector<V3> all(n);
    for (int i = 0; i < n; i++) all[i] = {Get<float>(c203, 2 + 12 * i), Get<float>(c203, 6 + 12 * i), Get<float>(c203, 10 + 12 * i)};
    std::vector<std::vector<std::pair<int, float>>> w(n);
    uint32_t count = Get<uint32_t>(c208, 72);
    size_t p = 76;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 62 > c208.size()) return false;
        t.bones.emplace_back(c208.begin() + p, c208.begin() + p + 60);
        uint16_t nw = Get<uint16_t>(c208, p + 60);
        p += 62;
        for (uint16_t j = 0; j < nw; j++) {
            uint32_t v = Get<uint32_t>(c208, p + 8 * j);
            float x = Get<float>(c208, p + 8 * j + 4);
            if (v < n) w[v].push_back({int(k), x});
        }
        p += 8 * size_t(nw);
    }
    t.head208.assign(c208.begin(), c208.begin() + 64);
    t.tail208.assign(c208.begin() + p, c208.end());
    if (t.tail208.size() < 20) return false;
    for (int i = 0; i < n; i++)
        if (!w[i].empty()) t.points.push_back(all[i]), t.weights.push_back(w[i]);
    t.head200 = c200;
    return !t.points.empty();
}

bool WriteFile(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}
}  // namespace

// Makes characters\chief from the zip. Torque's body: his mesh's chunks 0x200, 0x203, 0x208 from the Player package.
bool Make(const fs::path& zip, const fs::path& out, const std::vector<uint8_t>& c200, const std::vector<uint8_t>& c203, const std::vector<uint8_t>& c208) {
    std::map<std::string, Bytes> files;
    if (!ReadZip(zip, files)) {
        LOGR("chief: %s could not be read as a zip", zip.string().c_str());
        return false;
    }
    const Bytes* fbx = nullptr;
    for (auto& [name, b] : files)
        if (name.size() > 4 && name.substr(name.size() - 4) == ".fbx") fbx = &b;
    std::vector<Part> parts;
    if (!fbx || !LoadParts(*fbx, parts)) {
        LOGR("chief: no model (a binary .fbx) could be read from %s", zip.string().c_str());
        return false;
    }
    Torque torque;
    if (!ReadTorque(c200, c203, c208, torque)) {
        LOGR("chief: Torque's body is not laid out as expected");
        return false;
    }
    // places: by material, Helmet, Torso, Arms, Legs first, then others in the order met (four at most: Torque's)
    std::vector<std::string> places;
    for (const char* known : {"helmet", "torso", "arms", "legs"})
        for (const Part& p : parts)
            if (Lower(p.material) == known && std::find(places.begin(), places.end(), p.material) == places.end()) places.push_back(p.material);
    for (const Part& p : parts)
        if (std::find(places.begin(), places.end(), p.material) == places.end()) places.push_back(p.material);
    if (places.size() > 4) {
        LOGR("chief: the model has %zu materials, at most four are drawn", places.size());
        return false;
    }
    // height and scale
    double lo = 1e30, hi = -1e30, tlo = 1e30, thi = -1e30;
    for (const Part& p : parts) for (const V3& v : p.points) lo = std::min(lo, v[1]), hi = std::max(hi, v[1]);
    for (const V3& v : torque.points) tlo = std::min(tlo, v[1]), thi = std::max(thi, v[1]);
    if (hi <= lo) return false;
    double scale = (thi - tlo) / (hi - lo);
    // the arms, as Torque holds his
    const double kDown = 12.0 * 3.14159265358979323846 / 180;
    const double arm_length = 0.60 / scale;
    struct Pose {
        V3 pivot{}, target{};
        M3 turn = Identity();
        double length = 1;
    } pose[2];
    std::vector<V3> arms;
    for (const Part& p : parts)
        if (Lower(p.name).rfind("arms", 0) == 0) arms.insert(arms.end(), p.points.begin(), p.points.end());
    for (int s = 0; s < 2; s++) {
        double side = s ? 1 : -1;
        std::vector<V3> pts;
        for (const V3& v : arms)
            if (v[0] * side > 0.029 * (hi - lo)) pts.push_back(v);
        if (pts.size() < 10) continue;
        V3 c{0, 0, 0};
        for (const V3& v : pts) c = AddV(c, v);
        c = Mul(c, 1.0 / double(pts.size()));
        double cov[3][3] = {};
        for (const V3& v : pts) {
            V3 d = Sub(v, c);
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) cov[i][j] += d[i] * d[j];
        }
        V3 axis{1, 0, 0};
        for (int it = 0; it < 100; it++) {
            V3 nx{cov[0][0] * axis[0] + cov[0][1] * axis[1] + cov[0][2] * axis[2], cov[1][0] * axis[0] + cov[1][1] * axis[1] + cov[1][2] * axis[2],
                  cov[2][0] * axis[0] + cov[2][1] * axis[1] + cov[2][2] * axis[2]};
            double L = Len(nx);
            if (L < 1e-12) break;
            axis = Mul(nx, 1 / L);
        }
        std::vector<double> proj;
        for (const V3& v : pts) proj.push_back(Dot(Sub(v, c), axis));
        size_t imax = size_t(std::max_element(proj.begin(), proj.end()) - proj.begin()), imin = size_t(std::min_element(proj.begin(), proj.end()) - proj.begin());
        if (pts[imax][0] * side < pts[imin][0] * side) {
            axis = Mul(axis, -1);
            for (double& x : proj) x = -x;
        }
        std::vector<double> sorted = proj;
        std::sort(sorted.begin(), sorted.end());
        double p3 = sorted[size_t(0.03 * double(sorted.size() - 1))], p995 = sorted[size_t(0.995 * double(sorted.size() - 1))];
        Pose& ps = pose[s];
        ps.pivot = AddV(c, Mul(axis, p3));
        ps.target = {side * std::cos(kDown), -std::sin(kDown), 0};
        V3 k = Cross(axis, ps.target);
        double sn = Len(k), cs = Dot(axis, ps.target);
        if (sn > 1e-9) {
            k = Mul(k, 1 / sn);
            double ang = std::atan2(sn, cs);
            M3 K = {{{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}}, KK = MulM(K, K);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) ps.turn[i][j] = (i == j ? 1 : 0) + std::sin(ang) * K[i][j] + (1 - std::cos(ang)) * KK[i][j];
        }
        ps.length = std::max(1e-6, p995 - p3);
    }
    // vertices (part, point, uv) and triangles
    struct Vert {
        V3 pos, normal{0, 0, 0};
        double u = 0, v = 0;
        size_t part = 0;
        int64_t point = 0;
    };
    std::vector<Vert> verts;
    std::vector<Tri> tris;
    for (size_t pi = 0; pi < parts.size(); pi++) {
        const Part& part = parts[pi];
        bool is_arm = Lower(part.name).rfind("arms", 0) == 0;
        int place = int(std::find(places.begin(), places.end(), part.material) - places.begin());
        std::vector<V3> pts = part.points;
        if (is_arm)
            for (V3& p : pts) {
                const Pose& ps = pose[p[0] < 0 ? 0 : 1];
                V3 q = Apply(ps.turn, Sub(p, ps.pivot));
                double along = Dot(q, ps.target);
                if (along > 0) q = AddV(q, Mul(ps.target, along * (arm_length / ps.length - 1)));
                p = AddV(ps.pivot, q);
            }
        std::map<std::tuple<int64_t, int64_t, int64_t>, int> index;
        std::vector<int> poly;
        for (size_t j = 0; j < part.polys.size(); j++) {
            int64_t x = part.polys[j], cp = x >= 0 ? x : ~x;
            if (cp < 0 || size_t(cp) >= pts.size()) return false;
            int64_t ui = part.uv_index.empty() ? int64_t(j) : (j < part.uv_index.size() ? part.uv_index[j] : 0);
            double u = 2 * size_t(ui) + 1 < part.uv.size() ? part.uv[2 * size_t(ui)] : 0, v = 2 * size_t(ui) + 1 < part.uv.size() ? part.uv[2 * size_t(ui) + 1] : 0;
            auto key = std::make_tuple(cp, int64_t(std::llround(u * 1e6)), int64_t(std::llround(v * 1e6)));
            auto it = index.find(key);
            int vi;
            if (it == index.end()) {
                vi = int(verts.size());
                index[key] = vi;
                Vert nv;
                nv.pos = pts[size_t(cp)], nv.u = u, nv.v = v, nv.part = pi, nv.point = cp;
                verts.push_back(nv);
            } else vi = it->second;
            size_t ni = part.by_point ? size_t(cp) : j;
            if (3 * ni + 2 < part.normals.size()) verts[vi].normal = AddV(verts[vi].normal, {part.normals[3 * ni], part.normals[3 * ni + 1], part.normals[3 * ni + 2]});
            poly.push_back(vi);
            if (x < 0) {
                for (size_t q = 1; q + 1 < poly.size(); q++) tris.push_back({place, {poly[0], poly[q], poly[q + 1]}});
                poly.clear();
            }
        }
    }
    size_t before_v = verts.size(), before_t = tris.size();
    {
        std::vector<V3> P;
        for (const Vert& v : verts) P.push_back(v.pos);
        std::vector<int> kept;
        Decimate(P, tris, 6000, 5600, kept);
        std::vector<Vert> nv;
        for (int k : kept) nv.push_back(verts[size_t(k)]);
        verts = std::move(nv);
    }
    LOGR("chief: %zu parts, %zu vertices and %zu triangles, %zu and %zu after lightening", parts.size(), before_v, before_t, verts.size(), tris.size());
    // the body's frame: Torque's height, centred, torso at his depth, facing his way
    {
        double minx = 1e30, maxx = -1e30, miny = 1e30;
        for (Vert& v : verts) {
            v.pos = Mul(v.pos, scale);
            minx = std::min(minx, v.pos[0]), maxx = std::max(maxx, v.pos[0]), miny = std::min(miny, v.pos[1]);
        }
        double tz = 0, mz = 0;
        int tn = 0, mn = 0;
        for (const V3& p : torque.points)
            if (p[1] > 0.9 && p[1] < 1.3 && std::fabs(p[0]) < 0.25) tz += p[2], tn++;
        for (Vert& v : verts) {
            v.pos[0] -= (minx + maxx) / 2, v.pos[1] -= miny;
            if (v.pos[1] > 0.9 && v.pos[1] < 1.3 && std::fabs(v.pos[0]) < 0.25) mz += v.pos[2], mn++;
        }
        double shift = (tn ? tz / tn : 0) - (mn ? mz / mn : 0);
        for (Vert& v : verts) {
            v.pos[2] += shift;
            v.pos[0] = -v.pos[0], v.pos[2] = -v.pos[2];  // turned round: the model faces +Z, Torque -Z
            v.normal[0] = -v.normal[0], v.normal[2] = -v.normal[2];
            double L = Len(v.normal);
            v.normal = L > 1e-9 ? Mul(v.normal, 1 / L) : V3{0, 1, 0};
        }
    }
    // base vertices (one per point of each part) first, then the copies at uv seams
    std::vector<int> order, base_of(verts.size(), -1);
    std::map<std::pair<size_t, int64_t>, int> first;
    std::vector<int> dups_old;
    for (size_t i = 0; i < verts.size(); i++) {
        auto key = std::make_pair(verts[i].part, verts[i].point);
        auto it = first.find(key);
        if (it == first.end()) first[key] = int(i), order.push_back(int(i));
        else dups_old.push_back(int(i)), base_of[i] = it->second;
    }
    size_t nbase = order.size();
    order.insert(order.end(), dups_old.begin(), dups_old.end());
    size_t n = order.size();
    if (n >= 32767) return false;
    std::vector<int> new_of(verts.size());
    for (size_t k = 0; k < n; k++) new_of[size_t(order[k])] = int(k);
    // weights: the four nearest of Torque's skinned vertices, the three largest kept
    size_t nb = torque.bones.size();
    std::vector<std::vector<std::pair<int, float>>> weight(nbase);
    for (size_t k = 0; k < nbase; k++) {
        const V3& p = verts[size_t(order[k])].pos;
        std::array<std::pair<double, int>, 4> best;
        best.fill({1e30, -1});
        for (size_t t = 0; t < torque.points.size(); t++) {
            V3 d = Sub(p, torque.points[t]);
            double dd = Dot(d, d);
            if (dd < best[3].first) {
                best[3] = {dd, int(t)};
                std::sort(best.begin(), best.end());
            }
        }
        std::vector<double> w(nb, 0.0);
        double total = 0;
        for (auto& [dd, t] : best) {
            if (t < 0) continue;
            double iw = 1 / (std::sqrt(dd) + 1e-4);
            total += iw;
            for (auto& [bone, x] : torque.weights[size_t(t)]) w[size_t(bone)] += iw * x;
        }
        std::vector<std::pair<double, int>> sorted;
        for (size_t b = 0; b < nb; b++)
            if (w[b] > 0) sorted.push_back({w[b] / total, int(b)});
        std::sort(sorted.rbegin(), sorted.rend());
        if (sorted.size() > 3) sorted.resize(3);
        double sum = 0;
        for (auto& [x, b] : sorted) if (x >= 0.02) sum += x;
        for (auto& [x, b] : sorted)
            if (x >= 0.02 && sum > 0) weight[k].push_back({b, float(x / sum)});
        if (weight[k].empty() && !sorted.empty()) weight[k].push_back({sorted[0].second, 1.0f});
    }
    // triangles: front faces clockwise seen from outside
    for (Tri& t : tris) {
        const Vert &a = verts[size_t(t.v[0])], &b = verts[size_t(t.v[1])], &c = verts[size_t(t.v[2])];
        V3 cr = Cross(Sub(b.pos, a.pos), Sub(c.pos, a.pos));
        if (Dot(cr, AddV(AddV(a.normal, b.normal), c.normal)) > 0) std::swap(t.v[1], t.v[2]);
        for (int& x : t.v) x = new_of[size_t(x)];
    }
    auto uv_of = [&](int k) { const Vert& v = verts[size_t(order[size_t(k)])]; return std::make_pair(float(v.u), float(1.0 - v.v)); };
    // 0x203
    Bytes c203o;
    Add<uint16_t>(c203o, uint16_t(n));
    for (size_t k = 0; k < n; k++) for (double x : verts[size_t(order[k])].pos) Add<float>(c203o, float(x));
    for (size_t k = 0; k < n; k++) {
        for (double x : verts[size_t(order[k])].normal) Add<int8_t>(c203o, int8_t(std::lround(std::clamp(x, -1.0, 1.0) * 127)));
        Add<uint8_t>(c203o, 0x7F);
    }
    // 0x204
    Bytes c204o;
    Add<uint32_t>(c204o, uint32_t(tris.size()));
    for (const Tri& t : tris) {
        Add<uint16_t>(c204o, 3), Add<uint16_t>(c204o, uint16_t(t.place));
        for (int x : t.v) Add<uint16_t>(c204o, uint16_t(x));
        Add<uint16_t>(c204o, 0);
    }
    for (const Tri& t : tris)
        for (int x : t.v) {
            auto [u, v] = uv_of(x);
            Add<float>(c204o, u), Add<float>(c204o, v);
        }
    // 0x208
    Bytes c208o = torque.head208;
    Add<uint32_t>(c208o, 1), Add<uint32_t>(c208o, uint32_t(n)), Add<uint32_t>(c208o, uint32_t(nb));
    for (size_t b = 0; b < nb; b++) {
        c208o.insert(c208o.end(), torque.bones[b].begin(), torque.bones[b].end());
        std::vector<std::pair<uint32_t, float>> ws;
        for (size_t k = 0; k < nbase; k++)
            for (auto& [bone, x] : weight[k])
                if (size_t(bone) == b) ws.push_back({uint32_t(k), x});
        Add<uint16_t>(c208o, uint16_t(ws.size()));
        for (auto& [v, x] : ws) Add<uint32_t>(c208o, v), Add<float>(c208o, x);
    }
    {
        Bytes tail = torque.tail208;
        uint32_t nv32 = uint32_t(n), nt32 = uint32_t(tris.size());
        std::memcpy(tail.data() + 8, &nv32, 4);
        std::memcpy(tail.data() + 16, &nt32, 4);
        c208o.insert(c208o.end(), tail.begin(), tail.end());
    }
    // 0x211: seam copies, uvs, strips per place
    Bytes c211o;
    Add<uint32_t>(c211o, 1), Add<uint32_t>(c211o, uint32_t(n - nbase));
    for (size_t k = nbase; k < n; k++) {
        Add<uint16_t>(c211o, uint16_t(new_of[size_t(base_of[size_t(order[k])])])), Add<uint16_t>(c211o, uint16_t(k));
    }
    Add<uint32_t>(c211o, uint32_t(n));
    for (size_t k = 0; k < n; k++) {
        auto [u, v] = uv_of(int(k));
        Add<float>(c211o, u), Add<float>(c211o, v);
    }
    std::set<int> used_places;
    for (const Tri& t : tris) used_places.insert(t.place);
    Add<uint32_t>(c211o, uint32_t(used_places.size()));
    size_t total_indices = 0;
    for (int pl : used_places) {
        std::vector<std::array<int, 3>> group;
        for (const Tri& t : tris)
            if (t.place == pl) group.push_back({t.v[0], t.v[1], t.v[2]});
        std::map<std::pair<int, int>, std::vector<int>> by_edge;
        for (size_t k = 0; k < group.size(); k++)
            for (int e = 0; e < 3; e++) by_edge[{group[k][size_t(e)], group[k][size_t((e + 1) % 3)]}].push_back(int(k));
        std::vector<bool> used(group.size(), false);
        auto third = [&](int k, int x, int y) {
            const auto& g = group[size_t(k)];
            for (int e = 0; e < 3; e++)
                if (g[size_t(e)] == x && g[size_t((e + 1) % 3)] == y) return g[size_t((e + 2) % 3)];
            return -1;
        };
        // (each strip starts at the triangle with the fewest free neighbours, and goes on to the free neighbour that
        // has the fewest: longer strips, fewer indices)
        auto free_around = [&](int k) {
            int c = 0;
            const auto& g = group[size_t(k)];
            for (int e = 0; e < 3; e++) {
                auto it = by_edge.find({g[size_t((e + 1) % 3)], g[size_t(e)]});
                if (it != by_edge.end())
                    for (int o : it->second)
                        if (!used[size_t(o)]) c++;
            }
            return c;
        };
        std::vector<uint16_t> strip;
        for (;;) {
            int s = -1, fewest = 99;
            for (size_t k = 0; k < group.size(); k++)
                if (!used[k]) {
                    int f = free_around(int(k));
                    if (f < fewest) fewest = f, s = int(k);
                    if (f == 0) break;
                }
            if (s < 0) break;
            used[size_t(s)] = true;
            std::vector<int> run = {group[size_t(s)][0], group[size_t(s)][1], group[size_t(s)][2]};
            for (;;) {
                int x = run[run.size() - 2], y = run.back();
                bool odd = (run.size() - 2) % 2 == 1;
                std::pair<int, int> want = odd ? std::make_pair(y, x) : std::make_pair(x, y);
                int next = -1, least = 99;
                auto it = by_edge.find(want);
                if (it != by_edge.end())
                    for (int k : it->second)
                        if (!used[size_t(k)]) {
                            int f = free_around(k);
                            if (f < least) least = f, next = k;
                        }
                if (next < 0) break;
                used[size_t(next)] = true;
                run.push_back(third(next, want.first, want.second));
            }
            if (!strip.empty()) {
                strip.push_back(strip.back()), strip.push_back(uint16_t(run[0]));
                if (strip.size() % 2 == 1) strip.push_back(uint16_t(run[0]));
            }
            for (int x : run) strip.push_back(uint16_t(x));
        }
        Add<uint32_t>(c211o, uint32_t(pl)), Add<uint32_t>(c211o, uint32_t(group.size())), Add<uint32_t>(c211o, uint32_t(strip.size()));
        for (uint16_t x : strip) Add<uint16_t>(c211o, x);
        total_indices += strip.size();
    }
    // 0x216
    Bytes c216o;
    Add<uint16_t>(c216o, uint16_t(n));
    for (size_t k = 0; k < nbase; k++) Add<int16_t>(c216o, int16_t(k + 1));
    for (size_t k = nbase; k < n; k++) Add<int16_t>(c216o, int16_t(-(new_of[size_t(base_of[size_t(order[k])])] + 1)));
    // 0x200: a plain body head (as Blackmore's) with Torque's first materials, one a place
    Bytes c200o;
    {
        uint32_t nm = Get<uint32_t>(torque.head200, 8);
        uint32_t mats = uint32_t(std::max<size_t>(used_places.empty() ? 1 : size_t(*used_places.rbegin()) + 1, 1));
        if (mats > nm) return false;
        const uint8_t flags[8] = {0x00, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00};
        c200o.assign(flags, flags + 8);
        Add<uint32_t>(c200o, mats);
        for (uint32_t k = 0; k < mats; k++) Add<uint32_t>(c200o, Get<uint32_t>(torque.head200, 12 + 4 * k));
        Add<uint32_t>(c200o, 0x7D), Add<float>(c200o, 1.0f), Add<uint32_t>(c200o, 10), Add<uint32_t>(c200o, 4);
    }
    LOGR("chief: %zu vertices (%zu skinned), %zu triangles, %zu strip indices, %zu places", n, nbase, tris.size(), total_indices, used_places.size());
    if (total_indices > 32700) {  // (32,671 ran well; 44,562 crashed the game. The game's own largest body has 9,596)
        LOGR("chief: too many strip indices for the game; not made");
        return false;
    }
    // the textures, one a place
    std::error_code ec;
    fs::create_directories(out, ec);
    for (size_t pl = 0; pl < places.size(); pl++) {
        std::string tex;
        for (const Part& p : parts)
            if (p.material == places[pl] && !p.texture.empty()) tex = p.texture;
        std::string base = Lower(tex.substr(tex.find_last_of("/\\") == std::string::npos ? 0 : tex.find_last_of("/\\") + 1));
        auto it = files.find(base);
        if (it == files.end()) it = files.find(Lower(places[pl]) + ".png");
        if (it == files.end()) {
            LOGR("chief: no texture for %s (%s)", places[pl].c_str(), tex.c_str());
            return false;
        }
        int w, h, ch;
        stbi_uc* px = stbi_load_from_memory(it->second.data(), int(it->second.size()), &w, &h, &ch, 4);
        if (!px) return false;
        Bytes dds(128, 0);
        auto put = [&](size_t at, uint32_t v) { std::memcpy(&dds[at], &v, 4); };
        put(0, 0x20534444), put(4, 124), put(8, 0x100F), put(12, uint32_t(h)), put(16, uint32_t(w)), put(20, uint32_t(w) * 4);
        put(76, 32), put(80, 0x41), put(88, 32), put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000), put(108, 0x1000);
        for (int i = 0; i < w * h; i++) dds.push_back(px[4 * i + 2]), dds.push_back(px[4 * i + 1]), dds.push_back(px[4 * i]), dds.push_back(px[4 * i + 3]);
        stbi_image_free(px);
        if (!WriteFile(out / ("chief_skin_" + std::to_string(pl) + ".dds"), dds)) return false;
    }
    const std::pair<uint16_t, const Bytes*> chunks[] = {{0x200, &c200o}, {0x203, &c203o}, {0x204, &c204o}, {0x208, &c208o}, {0x211, &c211o}, {0x216, &c216o}};
    for (auto& [kind, b] : chunks) {
        char name[32];
        std::snprintf(name, sizeof name, "chief_%03x.bin", kind);
        if (!WriteFile(out / name, *b)) return false;
    }
    LOGR("chief: made in %s", out.string().c_str());
    return true;
}
}  // namespace game_chief
