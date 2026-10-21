// Hand edits of a room's collision (collision_edits.h).
#include "core/rooms/collision_edits.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>

namespace studio {

namespace {

using P = std::array<float, 3>;

bool fail_with(std::string* why, const std::string& reason) {
    if (why != nullptr)
        *why = reason;
    return false;
}

const char* const kListWords[3] = {"ground", "wall", "ceiling"};

int list_of_word(const std::string& w) {
    for (int l = 0; l < 3; ++l) {
        if (w == kListWords[l])
            return l;
    }
    return -1;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names)
        out += (out.empty() ? "" : ",") + n;
    return out.empty() ? std::string("-") : out;
}

std::vector<std::string> split_names(const std::string& text) {
    std::vector<std::string> out;
    if (text == "-")
        return out;
    std::string cur;
    for (const char ch : text) {
        if (ch == ',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool is_gate(const std::string& name) {
    return name.size() == 6 && name.rfind("gate", 0) == 0 && name[4] >= '0' && name[4] <= '9' && name[5] >= '0' &&
           name[5] <= '9';
}

P sub(const P& a, const P& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
P cross(const P& a, const P& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const P& a, const P& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// a vector turned about x, then y, then z (degrees)
P turned(const std::array<float, 3>& turn, P p) {
    const float k = 3.14159265358979f / 180.0f;
    const float cx = std::cos(turn[0] * k), sx = std::sin(turn[0] * k);
    p = {p[0], p[1] * cx - p[2] * sx, p[1] * sx + p[2] * cx};
    const float cy = std::cos(turn[1] * k), sy = std::sin(turn[1] * k);
    p = {p[0] * cy + p[2] * sy, p[1], -p[0] * sy + p[2] * cy};
    const float cz = std::cos(turn[2] * k), sz = std::sin(turn[2] * k);
    return {p[0] * cz - p[1] * sz, p[0] * sz + p[1] * cz, p[2]};
}

// a shape's point placed: sized, turned about x, then y, then z, then moved
P place(const CollisionShape& s, const P& q) {
    const P p = turned(s.turn, {q[0] * s.size[0], q[1] * s.size[1], q[2] * s.size[2]});
    return {p[0] + s.at[0], p[1] + s.at[1], p[2] + s.at[2]};
}

void write_key(std::ostream& o, const FaceKey& k) {
    o << k.count;
    for (const auto& c : k)
        o << " " << c[0] << " " << c[1] << " " << c[2];
}

// a float as its shortest text that reads back the same (read_float), and read: a finite number, whole word
void write_float(std::ostream& o, float f) {
    char text[32];
    const auto r = std::to_chars(text, text + sizeof(text), std::isfinite(f) ? f : 0.0f);
    o.write(text, r.ptr - text);
}

bool read_float(std::istream& v, float* f) {
    std::string word;
    if (!(v >> word))
        return false;
    const auto r = std::from_chars(word.data(), word.data() + word.size(), *f);
    return r.ec == std::errc() && r.ptr == word.data() + word.size() && std::isfinite(*f);
}

bool read_key(std::istream& v, int list, FaceKey* k) {
    std::size_t n = 0;
    v >> n;
    if (!v || n < 3 || n > 4)
        return false;
    *k = FaceKey{};
    k->list = list;
    k->count = n;
    for (std::size_t i = 0; i < n; ++i)
        v >> k->corners[i][0] >> k->corners[i][1] >> k->corners[i][2];
    if (!v)
        return false;
    // as face_key orders them: the smallest first, the turn kept
    const auto end = k->corners.begin() + static_cast<std::ptrdiff_t>(n);
    std::rotate(k->corners.begin(), std::min_element(k->corners.begin(), end), end);
    return true;
}

// A face given to another list: a ground's slot 3 and last word are the nearest of the game's grounds' (as a
// made ground's); leaving the grounds, slot 3 names nothing (it names a record on a wall or ceiling).
void to_list(RoomFace* g, int from, int to, const RoomCollision& original) {
    if (to == from)
        return;
    if (to == kGround) {
        double best = -1;
        for (const auto& og : original.faces[kGround]) {
            const double dx = (og.bounds[0] + og.bounds[2]) / 2.0 - (g->bounds[0] + g->bounds[2]) / 2.0;
            const double dz = (og.bounds[1] + og.bounds[3]) / 2.0 - (g->bounds[1] + g->bounds[3]) / 2.0;
            const double dy = static_cast<double>(og.v[0][1]) - g->v[0][1];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d2 < best) {
                best = d2;
                g->slots[3] = og.slots[3];
                g->extra = og.extra;
            }
        }
    } else if (from == kGround) {
        g->slots[3] = 0xff;
    }
}

}  // namespace

std::array<float, 3> shape_point(const CollisionShape& s, const std::array<float, 3>& unit) {
    return place(s, unit);
}

std::array<float, 3> shape_axis(const CollisionShape& s, int axis) {
    P e{0, 0, 0};
    e[static_cast<std::size_t>(std::clamp(axis, 0, 2))] = 1;
    return turned(s.turn, e);
}

CollisionShape fit_shape(CollisionShape::Kind kind, const std::vector<std::array<float, 3>>& points,
                         const std::array<float, 3>& normal, const std::array<float, 3>& fallback) {
    CollisionShape s;
    s.kind = kind;
    s.size = kind == CollisionShape::box ? std::array<float, 3>{2, 2, 2} : std::array<float, 3>{4, 2, 4};
    s.at = fallback;
    if (points.empty())
        return s;
    if (kind == CollisionShape::plane) {
        // turned so its up is the faces' normal: about x by the tilt, then about y by the heading
        const float nl = std::sqrt(dot(normal, normal));
        const P n = nl > 1e-6f ? P{normal[0] / nl, normal[1] / nl, normal[2] / nl} : P{0, 1, 0};
        const float tilt = std::acos(std::clamp(n[1], -1.0f, 1.0f));
        const float heading = std::abs(std::sin(tilt)) > 1e-4f ? std::atan2(n[0], n[2]) : 0.0f;
        s.turn = {tilt * 57.2957795f, heading * 57.2957795f, 0};
        // its extent along its own two sides, at the faces' mean height over its plane
        const P a0 = shape_axis(s, 0), a1 = shape_axis(s, 1), a2 = shape_axis(s, 2);
        float lo0 = 1e30f, hi0 = -1e30f, lo2 = 1e30f, hi2 = -1e30f, up = 0;
        for (const auto& q : points) {
            const float e0 = dot(q, a0), e2 = dot(q, a2);
            lo0 = std::min(lo0, e0), hi0 = std::max(hi0, e0);
            lo2 = std::min(lo2, e2), hi2 = std::max(hi2, e2);
            up += dot(q, a1) / static_cast<float>(points.size());
        }
        const float m0 = (lo0 + hi0) / 2, m2 = (lo2 + hi2) / 2;
        s.size = {std::max(hi0 - lo0, 0.25f), 1, std::max(hi2 - lo2, 0.25f)};
        s.at = {a0[0] * m0 + a2[0] * m2 + a1[0] * up, a0[1] * m0 + a2[1] * m2 + a1[1] * up,
                a0[2] * m0 + a2[2] * m2 + a1[2] * up};
        return s;
    }
    P lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, mid{0, 0, 0};
    for (const auto& q : points) {
        for (std::size_t c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], q[c]), hi[c] = std::max(hi[c], q[c]);
            mid[c] += q[c] / static_cast<float>(points.size());
        }
    }
    const float dx = std::max(hi[0] - lo[0], 0.25f), dy = std::max(hi[1] - lo[1], 0.25f),
                dz = std::max(hi[2] - lo[2], 0.25f);
    s.at = {(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2};
    s.size = {dx, dy, dz};
    if (kind == CollisionShape::ramp || kind == CollisionShape::wedge) {
        // which way the faces rise: along x or z, and to which end
        float along_x = 0, along_z = 0;
        for (const auto& q : points) {
            along_x += (q[0] - mid[0]) * (q[1] - mid[1]);
            along_z += (q[2] - mid[2]) * (q[1] - mid[1]);
        }
        if (std::abs(along_x) > std::abs(along_z)) {
            s.turn = {0, along_x > 0 ? 90.0f : -90.0f, 0};
            s.size = {dz, dy, dx};
        } else {
            s.turn = {0, along_z > 0 ? 0.0f : 180.0f, 0};
        }
    }
    return s;
}

bool drop_to_ground(const RoomCollision& c, CollisionShape* s, const std::function<bool(int, std::size_t)>& skip) {
    // the gap under each point of its bottom; the shape moves by the smallest
    bool any = false;
    double move = 0;
    const P bottom[5] = {{0, 0, 0}, {-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}};
    for (const P& u : bottom) {
        const P q = place(*s, u);
        RayHit hit;
        // looking down from half a unit over it (a shape sunk a little still finds the floor it is in), else
        // from high above (a shape under the floor comes up onto it)
        if (!cast_ray(c, {q[0], q[1] + 0.5, q[2]}, {0, -1, 0}, skip, &hit) &&
            !cast_ray(c, {q[0], q[1] + 10000.0, q[2]}, {0, -1, 0}, skip, &hit))
            continue;
        if (hit.list != kGround)
            continue;  // only a floor holds it
        const double d = hit.at[1] - q[1];
        if (!any || d > move)
            move = d;
        any = true;
    }
    if (!any)
        return false;
    s->at[1] = static_cast<float>(s->at[1] + move);
    return true;
}

FaceKey face_key(int list, const RoomFace& f) {
    FaceKey k;
    k.list = list;
    k.count = std::min<std::size_t>(f.corners, 4);
    std::copy_n(f.v.begin(), k.count, k.corners.begin());
    if (k.count > 0) {
        const auto end = k.corners.begin() + static_cast<std::ptrdiff_t>(k.count);
        std::rotate(k.corners.begin(), std::min_element(k.corners.begin(), end), end);
    }
    return k;
}

const char* shape_word(CollisionShape::Kind k) {
    switch (k) {
    case CollisionShape::plane: return "plane";
    case CollisionShape::ramp: return "ramp";
    case CollisionShape::box: return "box";
    case CollisionShape::wedge: return "wedge";
    }
    return "plane";
}

std::vector<std::array<std::array<float, 3>, 3>> shape_triangles(const CollisionShape& s) {
    // the shape's faces at unit size (x and z from -0.5 to 0.5, y from 0 to 1), each turned outwards
    std::vector<std::vector<P>> faces;
    bool closed = false;
    P inside{0, 0.5f, 0};
    const P a{-0.5f, 0, -0.5f}, b{-0.5f, 0, 0.5f}, c{0.5f, 0, 0.5f}, d{0.5f, 0, -0.5f};  // the bottom
    const P e{-0.5f, 1, -0.5f}, f{-0.5f, 1, 0.5f}, g{0.5f, 1, 0.5f}, h{0.5f, 1, -0.5f};  // the top
    switch (s.kind) {
    case CollisionShape::plane: faces = {{a, b, c, d}}; break;  // facing up
    case CollisionShape::ramp: faces = {{a, f, g, d}}; break;   // facing up, towards its low end
    case CollisionShape::box:
        closed = true;
        faces = {{a, b, c, d}, {e, f, g, h}, {a, b, f, e}, {d, c, g, h}, {a, d, h, e}, {b, c, g, f}};
        break;
    case CollisionShape::wedge:
        closed = true;
        inside = {0, 0.25f, 0.25f};
        faces = {{a, f, g, d}, {a, b, c, d}, {b, c, g, f}, {a, b, f}, {d, c, g}};
        break;
    }
    std::vector<std::array<P, 3>> out;
    for (auto& face : faces) {
        if (closed) {
            P mid{0, 0, 0};
            const float n = static_cast<float>(face.size());
            for (const auto& q : face)
                mid = {mid[0] + q[0] / n, mid[1] + q[1] / n, mid[2] + q[2] / n};
            if (dot(cross(sub(face[1], face[0]), sub(face[2], face[0])), sub(mid, inside)) < 0)
                std::reverse(face.begin(), face.end());
        }
        std::vector<P> placed;
        for (const auto& q : face)
            placed.push_back(place(s, q));
        for (std::size_t i = 1; i + 1 < placed.size(); ++i) {
            out.push_back({placed[0], placed[i], placed[i + 1]});
            if (!closed && s.both_sides)
                out.push_back({placed[0], placed[i + 1], placed[i]});
        }
    }
    return out;
}

bool apply_collision_edits(RoomCollision* c, const RoomCollision& original, const CollisionEdits& e, float ground_limit,
                           EditsApplied* applied, std::string* why) {
    EditsApplied a;
    RoomCollision out = *c;
    for (auto& list : out.faces)
        list.clear();
    auto record_of = [&](const std::string& name) -> int {
        for (std::size_t k = 0; k < out.named.size(); ++k) {
            if (out.named[k].text() == name)
                return static_cast<int>(k);
        }
        NamedRecord n;
        std::copy(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(name.size(), 8)),
                  n.name.begin());
        out.named.push_back(n);
        return static_cast<int>(out.named.size() - 1);
    };

    // the room's faces: kept, removed, moved or renamed
    std::set<FaceKey> found;
    for (int l = 0; l < 3; ++l) {
        for (const RoomFace& f : c->faces[static_cast<std::size_t>(l)]) {
            const FaceKey key = face_key(l, f);
            const auto it = e.faces.find(key);
            if (it == e.faces.end()) {
                out.faces[static_cast<std::size_t>(l)].push_back(f);
                continue;
            }
            found.insert(key);
            const FaceEdit& fe = it->second;
            if (fe.removed) {
                ++a.removed;
                continue;
            }
            RoomFace g = f;
            const int to = fe.list >= 0 && fe.list < 3 ? fe.list : l;
            if (fe.renamed) {
                std::vector<int> gates, others;
                for (const auto& name : fe.names) {
                    if (name.empty() || name.size() > 8)
                        return fail_with(why, "the name \"" + name + "\" is not 1-8 characters");
                    (is_gate(name) ? gates : others).push_back(record_of(name));
                }
                if (gates.size() > 1 || others.size() > 2)
                    return fail_with(why, "a face holds one gate and two other names");
                for (std::size_t p = 0; p < 3; ++p)
                    g.slots[p] = 0xff;
                if (to != kGround)
                    g.slots[3] = 0xff;
                if (!gates.empty())
                    g.slots[0] = static_cast<std::uint8_t>(gates[0]);
                for (std::size_t k = 0; k < others.size(); ++k)
                    g.slots[1 + k] = static_cast<std::uint8_t>(others[k]);
                ++a.renamed;
            }
            if (to != l) {
                to_list(&g, l, to, original);
                ++a.moved;
            }
            out.faces[static_cast<std::size_t>(to)].push_back(g);
        }
    }
    a.unmatched = e.faces.size() - found.size();
    for (int l = 0; l < 3; ++l)
        a.shape_of[static_cast<std::size_t>(l)].assign(out.faces[static_cast<std::size_t>(l)].size(), -1);

    // the shapes, each made as a model's faces are (one at a time: no face joins two of them), then given
    // the kind the shape asks for
    std::size_t made_faces = 0;
    for (std::size_t i = 0; i < e.shapes.size(); ++i) {
        const CollisionShape& s = e.shapes[i];
        CollisionSource source;
        source.triangles = shape_triangles(s);
        CollisionRules rules;
        rules.ground_limit = ground_limit;
        if (!s.names.empty()) {
            for (std::size_t t = 0; t < source.triangles.size(); ++t)
                rules.names_by_triangle[t] = s.names;
        }
        for (const auto& n : out.named)
            rules.drop_original.insert(n.text());  // the room's own named faces are there already
        RoomCollision made;
        std::string problem;
        if (!build_collision(source, out, rules, &made, nullptr, &problem))
            return fail_with(why, "shape " + std::to_string(i + 1) + ": " + problem);
        out.named = made.named;
        for (int l = 0; l < 3; ++l) {
            for (RoomFace f : made.faces[static_cast<std::size_t>(l)]) {
                const int to = s.list >= 0 && s.list < 3 ? s.list : l;
                to_list(&f, l, to, original);
                f.flags = static_cast<std::uint16_t>(f.flags & ~0x8000u);  // the tree marks its runs again
                out.faces[static_cast<std::size_t>(to)].push_back(f);
                a.shape_of[static_cast<std::size_t>(to)].push_back(static_cast<int>(i));
                ++made_faces;
            }
        }
    }
    if (out.named.size() > 0xff)
        return fail_with(why, "more than 255 names");
    for (const auto& list : out.faces) {
        if (list.size() > 0x7fff)
            return fail_with(why, "more than 32,767 faces of one kind (a node's first face is a 16-bit number)");
    }

    if (a.removed + a.moved + a.renamed + made_faces > 0) {
        std::array<std::vector<std::size_t>, 3> order;
        if (!place_tree(&out, original, &order, why))
            return false;
        for (int l = 0; l < 3; ++l) {
            std::vector<int> now;
            for (const std::size_t before : order[static_cast<std::size_t>(l)])
                now.push_back(a.shape_of[static_cast<std::size_t>(l)][before]);
            a.shape_of[static_cast<std::size_t>(l)] = std::move(now);
        }
        *c = std::move(out);
    } else {
        for (int l = 0; l < 3; ++l)
            a.shape_of[static_cast<std::size_t>(l)].assign(c->faces[static_cast<std::size_t>(l)].size(), -1);
    }
    if (applied != nullptr)
        *applied = std::move(a);
    return true;
}

std::string edits_text(const CollisionEdits& e) {
    std::ostringstream o;
    for (const auto& [key, fe] : e.faces) {
        if (fe.removed) {
            o << "face-remove " << kListWords[key.list] << " ";
            write_key(o, key);
            o << "\n";
            continue;
        }
        if (fe.list >= 0 && fe.list < 3 && fe.list != key.list) {
            o << "face-kind " << kListWords[key.list] << " " << kListWords[fe.list] << " ";
            write_key(o, key);
            o << "\n";
        }
        if (fe.renamed) {
            o << "face-names " << kListWords[key.list] << " " << joined(fe.names) << " ";
            write_key(o, key);
            o << "\n";
        }
    }
    for (const auto& s : e.shapes) {
        o << "shape " << shape_word(s.kind) << " " << (s.both_sides ? "both" : "one") << " " << joined(s.names);
        for (const auto* v : {&s.at, &s.turn, &s.size}) {
            for (const float f : *v) {
                o << " ";
                write_float(o, f);
            }
        }
        if (s.list >= 0 && s.list < 3)
            o << " as " << kListWords[s.list];  // else each face by its slope
        o << "\n";
    }
    return o.str();
}

int read_edit_line(const std::string& word, const std::string& rest, CollisionEdits* e, std::string* why) {
    std::istringstream v(rest);
    if (word == "face-remove" || word == "face-kind" || word == "face-names") {
        std::string list_word, second;
        v >> list_word;
        const int list = list_of_word(list_word);
        if (list < 0)
            return fail_with(why, "a face's kind is ground, wall or ceiling"), -1;
        if (word != "face-remove")
            v >> second;
        const int to = word == "face-kind" ? list_of_word(second) : -1;
        if (word == "face-kind" && to < 0)
            return fail_with(why, "a face's kind is ground, wall or ceiling"), -1;
        FaceKey key;
        if (!read_key(v, list, &key))
            return fail_with(why, "a face is 3 or 4 corners of three numbers"), -1;
        FaceEdit& fe = e->faces[key];
        if (word == "face-remove") {
            fe.removed = true;
        } else if (word == "face-kind") {
            fe.list = to;
        } else {
            fe.renamed = true;
            fe.names = split_names(second);
        }
        return 1;
    }
    if (word == "shape") {
        CollisionShape s;
        std::string kind, sides, names;
        v >> kind >> sides >> names;
        bool known = false;
        for (const auto k : {CollisionShape::plane, CollisionShape::ramp, CollisionShape::box, CollisionShape::wedge}) {
            if (kind == shape_word(k))
                s.kind = k, known = true;
        }
        if (!known)
            return fail_with(why, "a shape is a plane, ramp, box or wedge"), -1;
        if (sides != "one" && sides != "both")
            return fail_with(why, "a shape is stood on from one side or both"), -1;
        s.both_sides = sides == "both";
        s.names = split_names(names);
        bool numbers = true;
        for (auto* p : {&s.at, &s.turn, &s.size}) {
            for (float& f : *p)
                numbers = numbers && read_float(v, &f);
        }
        if (!numbers)
            return fail_with(why, "a shape's place, turn and size are three numbers each"), -1;
        std::string as, list_word;
        if (v >> as) {
            v >> list_word;
            s.list = list_of_word(list_word);
            if (as != "as" || s.list < 0)
                return fail_with(why, "a shape's faces are made \"as ground\", \"as wall\" or \"as ceiling\""), -1;
        }
        e->shapes.push_back(s);
        return 1;
    }
    return 0;
}

}  // namespace studio
