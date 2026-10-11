// SPDX-License-Identifier: Apache-2.0
// Small, authored index-space fixtures and a reference independent of the
// renderer's dense-grid extraction/interpolation. Loaded against the module
// under test; no duplicate renderer implementation is compiled into the bridge.
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <oneapi/tbb/global_control.h>
#include <nlohmann/json.hpp>
#include "slg/scene/scene.h"
#include "slg/textures/texturedefs.h"
#include "slg/textures/densitygrid.h"

using Json = nlohmann::json;
using RGB = std::array<double, 3>;
using FixtureVec4fGrid = openvdb::Grid<openvdb::tree::Tree4<openvdb::Vec4f, 5, 4, 3>::Type>;
namespace fs = std::filesystem;

template<class Grid, class Value>
typename Grid::Ptr uniform(const std::string &name, const Value value) {
    auto grid = Grid::create(Value(0));
    grid->setName(name);
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->fill(openvdb::CoordBBox({0, 0, 0}, {7, 7, 7}), value, true);
    grid->tree().voxelizeActiveTiles();
    return grid;
}

void makeFixture(const fs::path &path) {
    auto last = uniform<openvdb::FloatGrid>("float-last-plane", 0.f);
    auto accessor = last->getAccessor();
    for (int y = 0; y < 8; ++y) for (int z = 0; z < 8; ++z)
        accessor.setValue({7, y, z}, 1.f);
    auto isolated = openvdb::FloatGrid::create(0.f);
    isolated->setName("float-isolated");
    isolated->tree().setValue({0, 0, 0}, 1.f);
    isolated->tree().setValueOff({-3, 0, 0}, .25f);
    isolated->tree().setValueOff({3, 0, 0}, .75f);
    auto negative = openvdb::FloatGrid::create(0.f);
    negative->setName("float-negative");
    for (int x = -8; x < 0; ++x) for (int y = -8; y < 0; ++y)
        for (int z = -8; z < 0; ++z)
            negative->tree().setValue({x, y, z}, float(x + 9));
    auto tile = openvdb::FloatGrid::create(0.f);
    tile->setName("float-tile-only");
    tile->tree().addTile(1, {0, 0, 0}, .5f, true);
    auto background = openvdb::FloatGrid::create(.125f);
    background->setName("float-background");
    background->tree().setValue({0, 0, 0}, .75f);
    auto rgba = FixtureVec4fGrid::create(openvdb::Vec4f(.03125f, .0625f, .125f, .25f));
    rgba->setName("vec4f");
    rgba->setGridClass(openvdb::GRID_FOG_VOLUME);
    rgba->fill(openvdb::CoordBBox({0, 0, 0}, {7, 7, 7}),
               openvdb::Vec4f(.125f, .25f, .5f, .75f), true);
    rgba->tree().voxelizeActiveTiles();
    openvdb::GridPtrVec grids{
        last, isolated, negative, tile, background,
        uniform<openvdb::FloatGrid>("float", .375f),
        uniform<openvdb::DoubleGrid>("double", .375),
        uniform<openvdb::Int32Grid>("int32", 3),
        uniform<openvdb::Int64Grid>("int64", int64_t(5)),
        uniform<openvdb::BoolGrid>("bool", true),
        uniform<openvdb::Vec3SGrid>("vec3f", openvdb::Vec3s(.125f, .25f, .5f)),
        uniform<openvdb::Vec3DGrid>("vec3d", openvdb::Vec3d(.125, .25, .5)),
        uniform<openvdb::Vec3IGrid>("vec3i", openvdb::Vec3i(1, 2, 3)), rgba};
    openvdb::io::File file(path.string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(grids);
    file.close();
}

void makeRGBAPostfilterFixture(const fs::path &path) {
    auto grid = FixtureVec4fGrid::create(openvdb::Vec4f(0.f));
    grid->setName("rgba");
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    auto transform = openvdb::math::Transform::createLinearTransform(.0625);
    transform->postTranslate(openvdb::Vec3d(-1., -1., -1.));
    grid->setTransform(transform);
    const openvdb::Vec4f strips[4]{
        {.125f, .25f, .5f, .5f}, {0.f, 0.f, 0.f, 0.f},
        {1e-7f, 2e-7f, 4e-7f, 5e-7f}, {.125f, .25f, .5f, .25f}};
    for (int z = 0; z < 32; ++z) for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            grid->tree().setValue({x, y, z}, strips[x / 8]);
    openvdb::io::File file(path.string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(openvdb::GridPtrVec{grid});
    file.close();
}

template<class Grid>
RGB scalarValue(const openvdb::GridBase::Ptr &grid, const openvdb::Coord &coord) {
    const double value = openvdb::gridPtrCast<Grid>(grid)->tree().getValue(coord);
    return {value, value, value};
}
template<class Grid>
RGB vectorValue(const openvdb::GridBase::Ptr &grid, const openvdb::Coord &coord) {
    const auto value = openvdb::gridPtrCast<Grid>(grid)->tree().getValue(coord);
    return {double(value.x()), double(value.y()), double(value.z())};
}
RGB voxelValue(const openvdb::GridBase::Ptr &grid, openvdb::Coord coord,
               const std::string &component, const Json &test) {
    // Finite dense texture edge access is clamped. Within the physical carrier,
    // sufficient read padding prevents this clamp from hiding authored voxels.
    for (unsigned axis = 0; axis < 3; ++axis) {
        const int lo = int(test.at("domain_min")[axis].get<double>());
        const int size = int(test.at("domain_extent")[axis].get<double>());
        if (test.value("wrap", "black") == "repeat") {
            const int remainder = (coord[axis] - lo) % size;
            coord[axis] = lo + (remainder < 0 ? remainder + size : remainder);
        } else {
            coord[axis] = std::clamp(coord[axis], lo, lo + size - 1);
        }
    }
    if (test.contains("override_data")) {
        const int x = coord.x() - int(test.at("domain_min")[0].get<double>());
        const int y = coord.y() - int(test.at("domain_min")[1].get<double>());
        const int z = coord.z() - int(test.at("domain_min")[2].get<double>());
        const int nx = int(test.at("domain_extent")[0].get<double>());
        const int ny = int(test.at("domain_extent")[1].get<double>());
        const double value = test.at("override_data")[(z * ny + y) * nx + x].get<double>();
        return {value, value, value};
    }
    if (component == "alpha") {
        const double alpha = grid->isType<FixtureVec4fGrid>() ?
            double(openvdb::gridPtrCast<FixtureVec4fGrid>(grid)->tree().getValue(coord)[3]) : 1.;
        return {alpha, alpha, alpha};
    }
    if (grid->isType<FixtureVec4fGrid>()) return vectorValue<FixtureVec4fGrid>(grid, coord);
    if (grid->isType<openvdb::FloatGrid>()) return scalarValue<openvdb::FloatGrid>(grid, coord);
    if (grid->isType<openvdb::DoubleGrid>()) return scalarValue<openvdb::DoubleGrid>(grid, coord);
    if (grid->isType<openvdb::Int32Grid>()) return scalarValue<openvdb::Int32Grid>(grid, coord);
    if (grid->isType<openvdb::Int64Grid>()) return scalarValue<openvdb::Int64Grid>(grid, coord);
    if (grid->isType<openvdb::BoolGrid>()) return scalarValue<openvdb::BoolGrid>(grid, coord);
    if (grid->isType<openvdb::Vec3SGrid>()) return vectorValue<openvdb::Vec3SGrid>(grid, coord);
    if (grid->isType<openvdb::Vec3DGrid>()) return vectorValue<openvdb::Vec3DGrid>(grid, coord);
    if (grid->isType<openvdb::Vec3IGrid>()) return vectorValue<openvdb::Vec3IGrid>(grid, coord);
    throw std::runtime_error("Unhandled oracle grid type " + grid->valueType());
}
// Convert discrete values before interpolation: integer/bool grids are numeric
// attributes, so fractional queries retain fractional values.
RGB boxReference(const openvdb::GridBase::Ptr &grid, const Json &point,
                 const std::string &component, const Json &test) {
    openvdb::Coord base;
    RGB fraction, result{0., 0., 0.};
    for (unsigned a = 0; a < 3; ++a) {
        const double p = point[a].get<double>();
        base[a] = int(std::floor(p));
        fraction[a] = p - base[a];
    }
    for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const double weight = (x ? fraction[0] : 1. - fraction[0]) *
                (y ? fraction[1] : 1. - fraction[1]) *
                (z ? fraction[2] : 1. - fraction[2]);
            const RGB value = voxelValue(grid, base + openvdb::Coord(x, y, z), component, test);
            for (unsigned c = 0; c < 3; ++c) result[c] += weight * value[c];
        }
    return result;
}

RGB cubicReference(const openvdb::GridBase::Ptr &grid, const Json &point,
                   const std::string &component, const Json &test) {
    openvdb::Coord base;
    double weights[3][4];
    RGB result{0., 0., 0.};
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double p = point[axis].get<double>();
        base[axis] = int(std::floor(p)) - 1;
        const double t = p - std::floor(p);
        weights[axis][0] = (1. - t) * (1. - t) * (1. - t) / 6.;
        weights[axis][1] = (3. * t * t * t - 6. * t * t + 4.) / 6.;
        weights[axis][2] = (-3. * t * t * t + 3. * t * t + 3. * t + 1.) / 6.;
        weights[axis][3] = t * t * t / 6.;
    }
    for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const double weight = weights[0][x] * weights[1][y] * weights[2][z];
            const auto value = voxelValue(grid, base + openvdb::Coord(x, y, z), component, test);
            for (unsigned c = 0; c < 3; ++c) result[c] += weight * value[c];
        }
    return result;
}

RGB reference(const openvdb::GridBase::Ptr &grid, const Json &test, const Json &sample) {
    const std::string wrap = test.value("wrap", "black");
    for (const auto &p : sample.at("normalized"))
        if ((wrap == "black" || wrap == "white") &&
            (p.get<double>() < 0. || p.get<double>() >= 1.)) {
            const double outside = wrap == "white" ? 1. : 0.;
            return {outside, outside, outside};
        }
    if (test.contains("override_constant")) {
        const double value = test.at("override_constant").get<double>();
        return {value, value, value};
    }
    Json point = sample.at("index");
    if (wrap == "clamp" || wrap == "repeat")
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double p = sample.at("normalized")[axis].get<double>();
            const double mapped = wrap == "clamp" ? std::clamp(p, 0., 1.) : p - std::floor(p);
            point[axis] = test.at("domain_min")[axis].get<double>() +
                          mapped * test.at("domain_extent")[axis].get<double>();
        }
    return test.value("interpolation", "linear") == "cubic" ?
        cubicReference(grid, point, test.value("component", "rgb"), test) :
        boxReference(grid, point, test.value("component", "rgb"), test);
}

double floatErrorBudget(const Json &test, const double expected) {
    // 64 positive single-precision weighted terms may differ by a few ULP from
    // the independent double oracle. Preserve an absolute floor near zero and
    // scale by eight float epsilons elsewhere; this never changes the bounds.
    return test.value("absolute_tolerance", 2e-6) +
        8. * std::numeric_limits<float>::epsilon() * std::abs(expected);
}

Json bounds(const openvdb::CoordBBox &bbox) {
    if (bbox.empty()) return nullptr;
    return {{"min", {bbox.min().x(), bbox.min().y(), bbox.min().z()}},
            {"max_inclusive", {bbox.max().x(), bbox.max().y(), bbox.max().z()}}};
}

// Authoring uses a separate process. Registering Blender's custom Vec4f type in
// the evaluator before native parsing would mask missing reader registration.
extern "C" int slc_vdb_make_grid_fixture(const char *outputPath) {
    try {
        oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
        openvdb::initialize();
        if (!FixtureVec4fGrid::isRegistered()) FixtureVec4fGrid::registerGrid();
        makeFixture(outputPath);
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 0;
    }
}

extern "C" int slc_vdb_make_rgba_postfilter_fixture(const char *outputPath) {
    try {
        oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
        openvdb::initialize();
        if (!FixtureVec4fGrid::isRegistered()) FixtureVec4fGrid::registerGrid();
        makeRGBAPostfilterFixture(outputPath);
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 0;
    }
}

extern "C" int slc_vdb_grid_contract(const char *requestPath, const char *outputPath) {
    Json output;
    try {
        oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
        openvdb::initialize();
        Json request; std::ifstream(requestPath) >> request;
        const fs::path fixture = request.at("fixture").get<std::string>();
        if (!fs::exists(fixture)) throw std::runtime_error("Fixture must be authored in a separate process");
        output["fixture_bytes"] = fs::file_size(fixture);
        output["records"] = Json::array();
        for (const auto &test : request.at("cases")) {
            Json record{{"name", test.at("name")}, {"grid", test.at("grid")},
                        {"samples", Json::array()}, {"passed", true}};
            try {
                auto props = std::make_unique<luxrays::Properties>();
                props->SetFromString(test.at("properties").get<std::string>());
                props->Set(luxrays::Property("scene.textures.test.openvdb.file")(fixture.string()));
                props->Set(luxrays::Property("scene.textures.test.openvdb.grid")(test.at("grid").get<std::string>()));
                slg::Scene scene;
                scene.Parse(props);
                // Native reader parses first and must register custom Vec4f.
                // The independent oracle never registers it in this process.
                openvdb::io::File file(fixture.string()); file.open();
                const auto grid = file.readGrid(test.at("grid").get<std::string>());
                file.close();
                openvdb::CoordBBox leaf; grid->baseTree().evalLeafBoundingBox(leaf);
                record["grid_type"] = grid->valueType();
                record["active_bounds"] = bounds(grid->evalActiveVoxelBoundingBox());
                record["leaf_bounds"] = bounds(leaf);
                record["active_voxels"] = grid->activeVoxelCount();
                record["leaf_count"] = grid->baseTree().leafCount();
                const auto &texture = scene.GetTextures().GetTexture("test");
                const auto &density = dynamic_cast<const slg::DensityGridTexture &>(texture);
                auto serialized = texture.ToProperties(scene.GetImageMaps(), true);
                slg::Scene restored;
                restored.Parse(serialized);
                const auto &restoredTexture = restored.GetTextures().GetTexture("test");
                record["serialization_preserved"] = true;
                slg::HitPoint hit; hit.Init();
                double observedMin = INFINITY, observedMax = -INFINITY;
                for (const auto &sample : test.at("samples")) {
                    const auto &p = sample.at("normalized");
                    hit.p = luxrays::Point(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                    const auto actual = texture.EvalSpectrumValue(hit);
                    const auto expected = reference(grid, test, sample);
                    observedMin = std::min(observedMin, *std::min_element(expected.begin(), expected.end()));
                    observedMax = std::max(observedMax, *std::max_element(expected.begin(), expected.end()));
                    const auto roundtrip = restoredTexture.EvalSpectrumValue(hit);
                    float upper = 0.f, lower = 0.f;
                    const luxrays::BBox region(hit.p, hit.p);
                    const bool hasBounds = density.GetMaxInWorldBBox(region, &upper) &&
                        density.GetMinInWorldBBox(region, &lower);
                    const float expectedY = luxrays::Spectrum(expected[0], expected[1], expected[2]).Y();
                    const bool conservative = hasBounds && std::isfinite(upper) && std::isfinite(lower) &&
                        double(lower) <= *std::min_element(expected.begin(), expected.end()) + 2e-6 &&
                        double(upper) >= *std::max_element(expected.begin(), expected.end()) - 2e-6;
                    double error = 0., serializationError = 0., normalizedError = 0.;
                    bool finiteValues = true;
                    for (unsigned c = 0; c < 3; ++c) {
                        finiteValues &= std::isfinite(actual.c[c]) && std::isfinite(roundtrip.c[c]) &&
                                        std::isfinite(expected[c]);
                        error = std::max(error, std::abs(double(actual.c[c]) - expected[c]));
                        normalizedError = std::max(normalizedError,
                            std::abs(double(actual.c[c]) - expected[c]) / floatErrorBudget(test, expected[c]));
                        serializationError = std::max(serializationError,
                            std::abs(double(actual.c[c]) - double(roundtrip.c[c])));
                    }
                    const bool serializationPassed = finiteValues && std::isfinite(serializationError) && serializationError <= 2e-6;
                    const bool passed = finiteValues && std::isfinite(error) && normalizedError <= 1. &&
                        serializationPassed && conservative;
                    record["samples"].push_back({{"index", sample.at("index")},
                        {"normalized", p}, {"expected", expected},
                        {"actual", {actual.c[0], actual.c[1], actual.c[2]}},
                        {"absolute_error", error}, {"roundtrip", {roundtrip.c[0], roundtrip.c[1], roundtrip.c[2]}},
                        {"serialization_error", serializationError},
                        {"finite_values", finiteValues}, {"normalized_oracle_error", normalizedError},
                        {"bounds_available", hasBounds}, {"lower_bound", lower}, {"upper_bound", upper},
                        {"expected_luminance", expectedY}, {"bounds_conservative", conservative}, {"passed", passed}});
                    if (!serializationPassed) record["serialization_preserved"] = false;
                    if (!passed) record["passed"] = false;
                }
                // Positive B-spline weights make extrema of the complete
                // stencil conservative. Probe whole-carrier and crossing boxes
                // with independent index-space reconstruction, including the
                // accepted P==0 and excluded P==1 BLACK-wrap boundaries.
                record["carrier_bounds"] = Json::array();
                const double carrierLo = test.at("carrier_normalized")[0].get<double>();
                const double carrierHi = test.at("carrier_normalized")[1].get<double>();
                const double width = carrierHi - carrierLo;
                for (const auto &range : {std::array<double, 2>{carrierLo, carrierHi},
                                         std::array<double, 2>{0., 0.},
                                         std::array<double, 2>{carrierLo - .125, carrierLo + .25 * width},
                                         std::array<double, 2>{carrierHi - .25 * width, carrierHi + .125}}) {
                    const luxrays::BBox region(luxrays::Point(range[0], range[0], range[0]),
                                               luxrays::Point(range[1], range[1], range[1]));
                    float upper = 0.f, lower = 0.f;
                    const bool available = density.GetMaxInWorldBBox(region, &upper) &&
                                           density.GetMinInWorldBBox(region, &lower);
                    double sampledMin = INFINITY, sampledMax = -INFINITY, maxError = 0., maxNormalizedError = 0.;
                    bool finiteValues = true;
                    for (int z = 0; z < 5; ++z) for (int y = 0; y < 5; ++y)
                        for (int x = 0; x < 5; ++x) {
                            Json sample{{"normalized", Json::array()}, {"index", Json::array()}};
                            const int indices[3]{x, y, z};
                            for (unsigned axis = 0; axis < 3; ++axis) {
                                const double p = range[0] + (range[1] - range[0]) * indices[axis] / 4.;
                                sample["normalized"].push_back(p);
                                sample["index"].push_back(test.at("domain_min")[axis].get<double>() +
                                    p * test.at("domain_extent")[axis].get<double>());
                            }
                            const auto expected = reference(grid, test, sample);
                            const auto &p = sample.at("normalized");
                            hit.p = luxrays::Point(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                            const auto actual = texture.EvalSpectrumValue(hit);
                            for (unsigned channel = 0; channel < 3; ++channel) {
                                finiteValues &= std::isfinite(expected[channel]) && std::isfinite(actual.c[channel]);
                                sampledMin = std::min(sampledMin, expected[channel]);
                                sampledMax = std::max(sampledMax, expected[channel]);
                                maxError = std::max(maxError, std::abs(double(actual.c[channel]) - expected[channel]));
                                maxNormalizedError = std::max(maxNormalizedError,
                                    std::abs(double(actual.c[channel]) - expected[channel]) /
                                    floatErrorBudget(test, expected[channel]));
                            }
                        }
                    const bool passed = available && std::isfinite(upper) && std::isfinite(lower) &&
                        double(lower) <= sampledMin + 2e-6 && double(upper) >= sampledMax - 2e-6 &&
                        finiteValues && std::isfinite(maxError) && maxNormalizedError <= 1.;
                    record["carrier_bounds"].push_back({{"normalized_range", range},
                        {"independent_samples", 125}, {"sampled_min", sampledMin}, {"sampled_max", sampledMax},
                        {"lower_bound", lower}, {"upper_bound", upper}, {"max_oracle_error", maxError},
                        {"finite_values", finiteValues}, {"max_normalized_oracle_error", maxNormalizedError}, {"passed", passed}});
                    if (!passed) record["passed"] = false;
                    observedMin = std::min(observedMin, sampledMin);
                    observedMax = std::max(observedMax, sampledMax);
                }
                float globalMax = 0.f, globalMin = 0.f;
                const luxrays::BBox unbounded(luxrays::Point(-INFINITY, -INFINITY, -INFINITY),
                                               luxrays::Point(INFINITY, INFINITY, INFINITY));
                const bool globalAvailable = density.GetMaxInWorldBBox(unbounded, &globalMax) &&
                                             density.GetMinInWorldBBox(unbounded, &globalMin);
                // The unbounded domain also includes the wrap's outside value.
                const std::string wrap = test.value("wrap", "black");
                if (wrap == "black" || wrap == "white") {
                    const double outside = wrap == "white" ? 1. : 0.;
                    observedMin = std::min(observedMin, outside);
                    observedMax = std::max(observedMax, outside);
                }
                const bool globalPassed = globalAvailable && std::isfinite(globalMin) && std::isfinite(globalMax) &&
                    double(globalMin) <= observedMin + 2e-6 && double(globalMax) >= observedMax - 2e-6;
                record["unbounded_box"] = {{"available", globalAvailable}, {"lower_bound", globalMin},
                    {"upper_bound", globalMax}, {"observed_min", observedMin},
                    {"observed_max", observedMax}, {"passed", globalPassed}};
                if (!globalPassed) record["passed"] = false;
            } catch (const std::exception &error) {
                record["error"] = error.what(); record["passed"] = false;
            }
            output["records"].push_back(record);
        }
        output["passed"] = std::all_of(output["records"].begin(), output["records"].end(),
            [](const Json &record) { return record.at("passed").get<bool>(); });
        output["complete"] = true;
        output["float_error_budget"] = {{"absolute_floor", 2e-6}, {"relative_float_epsilons", 8},
                                        {"float_epsilon", std::numeric_limits<float>::epsilon()}};
    } catch (const std::exception &error) {
        output["error"] = error.what(); output["passed"] = false; output["complete"] = false;
    }
    std::ofstream(outputPath) << output.dump(2) << "\n";
    return output.at("passed").get<bool>() ? 1 : 0;
}
