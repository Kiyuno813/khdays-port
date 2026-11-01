// The map workshop's data: project files, triangles deleted, pieces, models made one and written out.
#include "test.h"

#include "core/models/model_files.h"
#include "core/workshop/workshop.h"

#include <cmath>

using namespace studio;
namespace assets = khdays::assets;

namespace {

// A square of two triangles at x..x+1, z 0..1, height y, its corners its own.
assets::NeutralMesh square(float x, float y, const std::string& texture = std::string()) {
    assets::NeutralMesh mesh;
    mesh.texture_name = texture;
    for (const auto& p : {std::array<float, 3>{x, y, 0}, std::array<float, 3>{x, y, 1},
                          std::array<float, 3>{x + 1, y, 1}, std::array<float, 3>{x + 1, y, 0}}) {
        assets::NeutralVertex v;
        v.position = p;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

assets::ImportedModel model_of(std::vector<assets::NeutralMesh> meshes) {
    assets::ImportedModel m;
    m.model.palette.push_back(std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    m.model.meshes = std::move(meshes);
    return m;
}

}  // namespace

TEST(workshop_project_reads_back) {
    WorkshopProject p;
    WorkshopMap a;
    a.map = "tt08";
    a.deleted[kWorkModel] = {0, 1, 1, 1, 0, 1, 0};
    a.deleted[kWorkCollision] = {1};
    WorkshopMap b;
    b.map = "tt06";
    b.place.move = {1200, -50.5f, 3};
    b.place.turn = {0, 90, 0};
    b.shown = false;
    p.maps = {a, b};
    const std::string text = workshop_text(p);
    CHECK(text.find("delete model 1-3 5") != std::string::npos && text.find("delete collision 0") != std::string::npos);
    WorkshopProject back;
    std::string why;
    CHECK(read_workshop_text(text, &back, &why));
    CHECK(back.maps.size() == 2u && back.maps[0].map == "tt08" && back.maps[1].map == "tt06");
    CHECK(back.maps[0].deleted[kWorkModel] == (std::vector<std::uint8_t>{0, 1, 1, 1, 0, 1}));
    CHECK(back.maps[1].place.move[0] == 1200 && back.maps[1].place.move[1] == -50.5f &&
          back.maps[1].place.turn[1] == 90 && !back.maps[1].shown);
    // a file of something else is said so
    CHECK(!read_workshop_text("hello\n", &back, &why) && !why.empty());
    const auto file = studio_test::scratch("workshop") / "sub" / "open tt.khmap";
    CHECK(save_workshop(file, p, &why));
    CHECK(load_workshop(file, &back, &why) && back.maps.size() == 2u);
}

TEST(workshop_deletes_triangles_and_finds_pieces) {
    // two squares apart (two pieces), then one touching the first (same piece)
    const auto m = model_of({square(0, 0), square(5, 0), square(1, 0)});
    CHECK(triangle_count(m) == 6u);
    const auto pieces = connected_pieces(m);
    CHECK(pieces.size() == 6u && pieces[0] == pieces[1] && pieces[0] == pieces[4] && pieces[2] == pieces[3] &&
          pieces[0] != pieces[2]);
    // the second square gone whole, one triangle of the third
    const auto kept = without_deleted(m, {0, 0, 1, 1, 1, 0});
    CHECK(kept.model.meshes.size() == 2u && triangle_count(kept) == 3u);
    CHECK(kept.model.meshes[1].vertices.size() == 3u);  // only the corners still used
    for (const auto i : kept.model.meshes[1].indices)
        CHECK(i < 3u);
}

TEST(workshop_merges_skeletons_apart) {
    // a still model and one with a joint at palette 1: the second's joint moved past the first's
    auto a = model_of({square(0, 0)});
    a.model.palette.push_back(a.model.palette[0]);
    assets::ImportedJoint ja;
    ja.palette_index = 1;
    ja.node = 0;
    a.joints = {ja};
    a.nodes.resize(1);
    auto b = model_of({square(3, 0)});
    b.model.meshes[0].vertices[0].joints = {1, 0, 0, 0};
    b.model.palette.push_back(b.model.palette[0]);
    assets::ImportedJoint jb;
    jb.palette_index = 1;
    jb.node = 0;
    b.joints = {jb};
    b.nodes.resize(2);
    b.nodes[1].parent = 0;
    assets::ImportedAnimation spin;
    spin.tracks.resize(1);
    spin.tracks[0].node = 1;
    b.animations = {spin};
    const auto one = merged_models({a, b});
    CHECK(one.model.meshes.size() == 2u && one.model.palette.size() == 3u);
    CHECK(one.joints.size() == 2u && one.joints[1].palette_index == 2u && one.joints[1].node == 1);
    CHECK(one.nodes.size() == 3u && one.nodes[2].parent == 1);
    CHECK(one.animations.size() == 1u && one.animations[0].tracks[0].node == 2);
    CHECK(one.model.meshes[1].vertices[0].joints[0] == 2u && one.model.meshes[1].vertices[1].joints[0] == 0u);
}

TEST(workshop_exports_placed_maps_as_one_gltf) {
    WorkshopProject p;
    WorkshopMap a, b;
    a.map = "aa00";
    b.map = "bb00";
    b.place.move = {100, 0, 0};
    b.deleted[kWorkModel] = {1, 0};
    p.maps = {a, b};
    auto ma = model_of({square(0, 0, "aa.png")});
    assets::DecodedTexture t;
    t.width = t.height = 2;
    t.rgba.assign(16, 200);
    ma.textures.push_back({"aa.png", t});
    const auto mb = model_of({square(0, 0)});
    const auto ca = model_of({square(0, 0)});
    const auto folder = studio_test::scratch("workshop_export");
    std::vector<std::string> written;
    std::string why;
    CHECK(export_workshop(p, {&ma, &mb}, {&ca, nullptr}, folder, "open tt", &written, &why));
    CHECK(written.size() == 2u && std::filesystem::exists(folder / "open tt.gltf") &&
          std::filesystem::exists(folder / "open tt_collision.gltf") &&
          std::filesystem::exists(folder / "open tt_0.png"));
    assets::ImportedModel back;
    CHECK(import_model(folder / "open tt.gltf", &back, &why));
    CHECK(triangle_count(back) == 3u);  // two of the first, one of the second
    float most_x = -1e9f;
    for (const auto& mesh : back.model.meshes)
        for (const auto& v : mesh.vertices)
            most_x = std::max(most_x, v.position[0]);
    CHECK(std::fabs(most_x - 101.0f) < 1e-3f);  // the second moved by 100
    CHECK(!back.textures.empty());
    assets::ImportedModel floor;
    CHECK(import_model(folder / "open tt_collision.gltf", &floor, &why) && triangle_count(floor) == 2u);
    // a bad name is refused
    CHECK(!export_workshop(p, {&ma, &mb}, {}, folder, "a/b", &written, &why));
}
