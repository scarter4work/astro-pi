#include "catch_amalgamated.hpp"
#include "nukex/stretch/stretch_pipeline.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include "nukex/io/image.hpp"

using namespace nukex;

namespace {

// A simple test op that adds a constant
class AddOp : public StretchOp {
public:
    float offset = 0.0f;
    AddOp(float off, int pos) : offset(off) {
        enabled = true; position = pos; name = "Add";
        category = StretchCategory::PRIMARY;
    }
    void apply(Image& img) const override {
        img.apply([this](float x) { return x + offset; });
    }
    float apply_scalar(float x) const override { return x + offset; }
};

} // anonymous namespace

TEST_CASE("StretchPipeline: execute runs enabled ops in order", "[pipeline]") {
    StretchPipeline pipeline;
    pipeline.ops.push_back(std::make_unique<AddOp>(0.1f, 2));
    pipeline.ops.push_back(std::make_unique<AddOp>(0.2f, 1));

    Image img(4, 4, 1);
    img.fill(0.0f);
    pipeline.execute(img);

    // Position 1 (add 0.2) runs first, then position 2 (add 0.1) = 0.3
    REQUIRE(img.at(0, 0, 0) == Catch::Approx(0.3f));
}

TEST_CASE("StretchPipeline: disabled ops are skipped", "[pipeline]") {
    StretchPipeline pipeline;
    auto op = std::make_unique<AddOp>(0.5f, 1);
    op->enabled = false;
    pipeline.ops.push_back(std::move(op));

    Image img(4, 4, 1);
    img.fill(0.0f);
    pipeline.execute(img);

    REQUIRE(img.at(0, 0, 0) == Catch::Approx(0.0f));
}

TEST_CASE("StretchPipeline: quick_preview_stretch produces non-zero output", "[pipeline]") {
    Image img(16, 16, 1);
    // Simulate faint astronomical data
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            img.at(x, y, 0) = 0.001f * (x + y);

    auto preview = StretchPipeline::quick_preview_stretch(img, 500.0f);
    // Preview should be brighter than the linear input
    REQUIRE(preview.at(8, 8, 0) > img.at(8, 8, 0));
    // Input should be unchanged (operates on copy)
    REQUIRE(img.at(8, 8, 0) == Catch::Approx(0.001f * 16));
}

TEST_CASE("StretchPipeline: quick_preview_stretch maps 0->0", "[pipeline]") {
    Image img(4, 4, 1);
    img.fill(0.0f);
    auto preview = StretchPipeline::quick_preview_stretch(img, 500.0f);
    REQUIRE(preview.at(0, 0, 0) == Catch::Approx(0.0f));
}

TEST_CASE("StretchPipeline: advisory warns on SECONDARY before PRIMARY", "[pipeline]") {
    StretchPipeline pipeline;
    auto sec = std::make_unique<AddOp>(0.1f, 1);
    sec->category = StretchCategory::SECONDARY;
    sec->name = "Histogram";
    pipeline.ops.push_back(std::move(sec));

    auto prim = std::make_unique<AddOp>(0.2f, 2);
    prim->category = StretchCategory::PRIMARY;
    prim->name = "GHS";
    pipeline.ops.push_back(std::move(prim));

    auto warnings = pipeline.check_ordering();
    REQUIRE(warnings.size() >= 1);
}
