#include "asset/GltfImporter.h"
#include "asset/SceneImporter.h"
#include "render/Math.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
void Check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool Near(float a, float b, float epsilon = 0.0002f) {
    return std::abs(a - b) < epsilon;
}

struct Fixture {
    std::filesystem::path directory;  // Isolated temporary files removed after each test case.
    std::vector<std::uint8_t> binary; // Shared indexed position/normal/UV/tangent fixture buffer.
    Fixture() {
        static unsigned serial = 0;
        directory = std::filesystem::temp_directory_path() /
                    ("RenderAssetTest_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++serial));
        std::filesystem::create_directories(directory);
        const float attributes[] = {1,           0,
                                    0,           0,
                                    1,           0,
                                    0,           0,
                                    0,           1,
                                    1,           0,
                                    1,           1,
                                    0,           1,
                                    1,           0,
                                    0,           0,
                                    1,           0,
                                    0,           1,
                                    0.70710678f, -0.70710678f,
                                    0,           1,
                                    0.70710678f, -0.70710678f,
                                    0,           1,
                                    0.70710678f, -0.70710678f,
                                    0,           1};
        binary.resize(sizeof(attributes) + 6);
        std::memcpy(binary.data(), attributes, sizeof(attributes));
        const std::uint16_t indices[] = {0, 1, 2};
        std::memcpy(binary.data() + sizeof(attributes), indices, sizeof(indices));
        Write("mesh.bin", binary);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void Write(const std::string &name, const std::vector<std::uint8_t> &bytes) const {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Check(bool(file), "fixture binary write");
    }
    std::filesystem::path Json(const std::string &text, const char *filename = "scene.gltf") const {
        std::ofstream file(directory / filename, std::ios::binary);
        file << text;
        Check(bool(file), "fixture glTF write");
        return directory / filename;
    }
};

std::string Document(
    std::string material =
        R"({"name":"Paint","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.8],"metallicFactor":0.3,"roughnessFactor":0.25}})",
    std::string nodes =
        R"([{"name":"Parent","translation":[10,20,30],"children":[1]},{"name":"Mesh","mesh":0,"matrix":[0,2,0,0,-1,0,0,0,0,0,3,0,0,0,0,1]}])",
    std::string extra = "") {
    return R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin","byteLength":150}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":48},{"buffer":0,"byteOffset":144,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":4,"componentType":5123,"count":3,"type":"SCALAR"}],"materials":[)" +
           material +
           R"(],"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"TANGENT":3},"indices":4,"material":0}]}],"nodes":)" +
           nodes + R"(,"scenes":[{"nodes":[0]}],"scene":0)" + extra + "}";
}

void Replace(std::string &text, const std::string &from, const std::string &to) {
    const auto at = text.find(from);
    Check(at != std::string::npos, "fixture replacement exists");
    text.replace(at, from.size(), to);
}
std::string Base64(const std::vector<std::uint8_t> &bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned value = unsigned(bytes[i]) << 16 | (i + 1 < bytes.size() ? unsigned(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[value & 63] : '=';
    }
    return result;
}
void SavePng(const std::filesystem::path &path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
          "fixture WIC factory");
    Check(SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)),
          "fixture PNG stream");
    Check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)),
          "fixture PNG encoder");
    Check(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(2, 1)),
          "fixture PNG frame");
    // The PNG encoder accepts BGRA input; the importer must still expose canonical RGBA bytes.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(SUCCEEDED(frame->SetPixelFormat(&format)), "fixture BGRA format");
    Check(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "fixture PNG encoder changed requested BGRA pixel format");
    BYTE pixels[] = {0, 0, 255, 255, 0, 255, 0, 128};
    Check(SUCCEEDED(frame->WritePixels(1, 8, 8, pixels)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()),
          "fixture PNG pixels");
}
std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
template <class Function> void Rejects(Function function, const char *message, const char *expected = "glTF import") {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::strstr(error.what(), expected) != nullptr;
    }
    Check(rejected, message);
}

void BakesHierarchyAndAttributes() {
    Fixture fixture;
    const auto scene = render::ImportGltf(fixture.Json(Document()));
    Check(scene.triangles.size() == 1 && scene.attributes.size() == 1,
          "indexed mesh creates triangle and stable attributes");
    const auto &triangle = scene.triangles.front();
    Check(Near(triangle.a.x, 10) && Near(triangle.a.y, 22) && Near(triangle.a.z, 30),
          "hierarchy matrix bakes position");
    Check(Near(triangle.b.x, 9) && Near(triangle.b.y, 20), "column-major node matrix bakes second position");
    Check(triangle.a.w == 0 && triangle.b.w == 1, "GPU material and attribute indirection");
    const auto &attribute = scene.attributes.front();
    Check(Near(attribute.normal0.x, -0.894427f) && Near(attribute.normal0.y, 0.447214f),
          "normal uses inverse transpose under nonuniform scale");
    Check(Near(attribute.tangent.x, 0.447214f) && Near(attribute.tangent.y, 0.894427f),
          "authored tangent rotates in world space");
    Check(Near(attribute.uv01.z, 1) && Near(attribute.uv2.y, 1), "UVs preserve glTF coordinates");
    Check(scene.meshParts.size() == 1 && scene.meshParts[0].name == "Mesh/Triangle/0" &&
              scene.meshParts[0].triangleCount == 1,
          "primitive metadata retains stable source ranges");
    Check(scene.materialNames[0] == "Paint" && !scene.sourcePath.empty(),
          "asset names and origin survive ownership transfer");
}
void ConvertsMaterialExtensions() {
    Fixture fixture;
    const auto material =
        R"({"name":"CoatedGlass","doubleSided":true,"alphaMode":"MASK","alphaCutoff":0.35,"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.8],"metallicFactor":0.3,"roughnessFactor":0.25},"emissiveFactor":[0.1,0.2,0.3],"extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":0.7,"clearcoatRoughnessFactor":0.2},"KHR_materials_transmission":{"transmissionFactor":0.9},"KHR_materials_ior":{"ior":1.7},"KHR_materials_specular":{"specularFactor":0.4},"KHR_materials_sheen":{"sheenColorFactor":[0.3,0.6,0.9],"sheenRoughnessFactor":0.25},"KHR_materials_anisotropy":{"anisotropyStrength":0.8,"anisotropyRotation":0.7},"KHR_materials_emissive_strength":{"emissiveStrength":2},"KHR_materials_volume":{"attenuationColor":[0.8,0.6,0.4],"attenuationDistance":3,"thicknessFactor":1}}})";
    const auto scene = render::ImportGltf(fixture.Json(Document(material)));
    const auto &m = scene.materials[0];
    Check(Near(m.baseRoughness.x, 0.2f) && Near(m.baseRoughness.w, 0.25f) && Near(m.emissionMetallic.w, 0.3f),
          "linear glTF metallic roughness factors");
    Check(Near(m.emissionMetallic.z, 0.6f), "emissive strength multiplies radiance");
    Check(Near(m.transmissionIor.x, 0.9f) && Near(m.transmissionIor.y, 1.7f) && Near(m.transmissionIor.z, 0.7f) &&
              Near(m.transmissionIor.w, 0.8f),
          "transmission IOR and clearcoat map to GPU lanes");
    Check(Near(m.principled.x, 0.2f) && Near(m.principled.z, 0.8f) && Near(m.extras.x, 0.7f),
          "specular and anisotropy extension mapping");
    Check(m.flags.x == 0 && m.flags.y == 1 && m.flags.z == 1 && Near(m.extras.z, 0.35f) && Near(m.extras.w, 0.8f),
          "volume double-sided alpha-mask controls");
    Check(Near(m.medium.y, 0.6f) && Near(m.medium.w, 3), "volume attenuation remains explicit");
    // Neutral specular color still carries reflectance magnitude even when the runtime stores scalar tint.
    const auto grey = render::ImportGltf(
        fixture.Json(Document(R"({"extensions":{"KHR_materials_specular":{"specularColorFactor":[0.5,0.5,0.5]}}})")));
    Check(Near(grey.materials[0].principled.x, 0.25f), "neutral specular color attenuates Disney reflectance");
}
void ImportsTextureAndTransform() {
    Fixture fixture;
    SavePng(fixture.directory / "colors.png");
    const auto material =
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3],"rotation":1.57079632679}}}}})";
    const auto extra =
        R"(,"images":[{"uri":"colors.png"}],"samplers":[{"wrapS":33071,"wrapT":33648}],"textures":[{"source":0,"sampler":0}])";
    const auto scene = render::ImportGltf(fixture.Json(Document(material, R"([{"name":"Mesh","mesh":0}])", extra)));
    Check(scene.textureViews.size() == 1 && scene.packedTexels.size() == 2, "WIC PNG owns tightly packed texels");
    Check(scene.packedTexels[0] == 0xff0000ffu && scene.packedTexels[1] == 0x8000ff00u,
          "packed texels are RGBA8 without premature color-space conversion");
    const auto &view = scene.textureViews.front();
    Check(view.image.y == 2 && view.image.z == 1 && view.image.w == (1u | (2u << 2)),
          "sampler wrap modes survive import");
    Check(Near(view.offsetScale.x, 0.25f) && Near(view.offsetScale.w, 3) && Near(view.rotation.x, 1) &&
              Near(view.rotation.y, 0),
          "KHR texture transform is preserved");
    Check(scene.materials[0].textures.x == 0, "material binds texture-view index");
    // The extension's texCoord overrides the base texture view rather than adding a second UV requirement.
    auto overridden = std::string(material);
    Replace(overridden, R"("index":0,"extensions")", R"("index":0,"texCoord":1,"extensions")");
    Replace(overridden, R"("offset":[0.25,0.5])", R"("texCoord":0,"offset":[0.25,0.5])");
    Check(render::ImportGltf(fixture.Json(Document(overridden, R"([{"mesh":0}])", extra))).textureViews.size() == 1,
          "extension UV0 override supersedes base UV1");
}
void ImportsGlbAndDataUris() {
    Fixture fixture;
    auto dataDocument = Document();
    Replace(dataDocument, "mesh.bin", "data:application/octet-stream;base64," + Base64(fixture.binary));
    const auto dataScene = render::ImportGltf(fixture.Json(dataDocument, "embedded.gltf"));
    Check(dataScene.triangles.size() == 1, "base64 buffer URI imports");
    SavePng(fixture.directory / "colors.png");
    const auto png = Read(fixture.directory / "colors.png");
    auto imageDocument =
        Document(R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}})", R"([{"mesh":0}])",
                 R"(,"images":[{"uri":"data:image/png;base64,)" + Base64(png) + R"("}],"textures":[{"source":0}])");
    Check(render::ImportGltf(fixture.Json(imageDocument, "image-data.gltf")).packedTexels.size() == 2,
          "base64 image URI imports");
    // GLB 同时嵌入网格和 PNG，块对齐填充刻意与缓冲实际长度分离。
    auto bytes = fixture.binary;
    while (bytes.size() % 4)
        bytes.push_back(0);
    const auto pngOffset = bytes.size();
    bytes.insert(bytes.end(), png.begin(), png.end());
    auto json = Document(R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}})", R"([{"mesh":0}])",
                         R"(,"images":[{"bufferView":5,"mimeType":"image/png"}],"textures":[{"source":0}])");
    Replace(json, R"("uri":"mesh.bin","byteLength":150)", R"("byteLength":)" + std::to_string(bytes.size()));
    Replace(json, R"("byteLength":6}],"accessors")",
            R"("byteLength":6},{"buffer":0,"byteOffset":)" + std::to_string(pngOffset) + R"(,"byteLength":)" +
                std::to_string(png.size()) + R"(}],"accessors")");
    while (json.size() % 4)
        json += ' ';
    while (bytes.size() % 4)
        bytes.push_back(0);
    std::vector<std::uint8_t> glb;
    const auto word = [&](std::uint32_t v) {
        for (unsigned b = 0; b < 4; ++b)
            glb.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
    };
    word(0x46546c67);
    word(2);
    word(static_cast<std::uint32_t>(28 + json.size() + bytes.size()));
    word(static_cast<std::uint32_t>(json.size()));
    word(0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    word(static_cast<std::uint32_t>(bytes.size()));
    word(0x004e4942);
    glb.insert(glb.end(), bytes.begin(), bytes.end());
    fixture.Write("scene.glb", glb);
    const auto scene = render::ImportGltf(fixture.directory / "scene.glb");
    Check(scene.triangles.size() == 1 && scene.packedTexels.size() == 2,
          "GLB mesh and bufferView image import together");
    // 通用 GLB 入口同样建立文件级模型，内嵌图像不因分组而丢失。
    const auto model = render::ImportSceneFile(fixture.directory / "scene.glb");
    Check(model.meshParts.size() == 1 && model.meshParts[0].name == "scene" && model.meshParts[0].useSurfaceMaterials &&
              model.packedTexels.size() == 2,
          "GLB imports as a whole textured model");
    // Windows 原生路径必须支持超出 ANSI 代码页的目录名及其中的外部缓冲。
    const auto unicodeDirectory = fixture.directory / std::filesystem::path(std::u8string(u8"asset_\u6a21\u578b_\U0001f31f"));
    std::filesystem::create_directories(unicodeDirectory);
    std::filesystem::copy_file(fixture.Json(Document()), unicodeDirectory / "scene.gltf");
    std::filesystem::copy_file(fixture.directory / "mesh.bin", unicodeDirectory / "mesh.bin");
    Check(render::ImportGltf(unicodeDirectory / "scene.gltf").triangles.size() == 1,
          "Unicode asset directory preserves external buffer loading");
}
void GroupsSceneFilesAsWholeModels() {
    Fixture fixture;
    const auto nodes = R"([{"name":"Parent","children":[1,2]},{"name":"First","mesh":0},{"name":"Second","mesh":0,"translation":[2,0,0]}])";
    const auto path = fixture.Json(Document("{}", nodes), "whole-model.gltf");
    // 文件中多个源节点在解析层保留独立范围，编辑层统一归属一个完整模型。
    Check(render::ImportGltf(path).meshParts.size() == 2, "source fixture contains two independent mesh nodes");
    const auto model = render::ImportSceneFile(path);
    Check(model.meshParts.size() == 1 && model.meshParts[0].name == "whole-model" && model.meshParts[0].triangleCount == 2 &&
              model.meshParts[0].useSurfaceMaterials && Near(model.triangles[1].a.x - model.triangles[0].a.x, 2),
          "glTF source nodes become one model without losing their relative positions");
}
void ImportsNamedPunctualLights() {
    Fixture fixture;
    const auto nodes =
        R"([{"mesh":0,"translation":[1,2,3],"children":[1,2,3]},{"name":"SunNode","extensions":{"KHR_lights_punctual":{"light":0}}},{"name":"PointNode","translation":[4,0,0],"extensions":{"KHR_lights_punctual":{"light":1}}},{"name":"SpotNode","extensions":{"KHR_lights_punctual":{"light":2}}}])";
    const auto extra =
        R"(,"extensions":{"KHR_lights_punctual":{"lights":[{"name":"Sun","type":"directional","color":[0.2,0.3,0.4],"intensity":2},{"name":"Bulb","type":"point","intensity":20,"range":7},{"name":"Spot","type":"spot","spot":{"innerConeAngle":0.2,"outerConeAngle":0.5}}]}})";
    const auto scene = render::ImportGltf(fixture.Json(Document("{}", nodes, extra)));
    Check(scene.lights.size() == 3 && scene.lightNames[0] == "SunNode/Sun", "named lights preserve node identity");
    Check(scene.lights[0].positionType.w == 0 && Near(scene.lights[0].directionRange.z, -1),
          "glTF local negative Z defines emission direction");
    Check(scene.lights[1].positionType.w == 1 && Near(scene.lights[1].positionType.x, 5) &&
              Near(scene.lights[1].directionRange.w, 7),
          "point light inherits parent translation and range");
    Check(scene.lights[2].positionType.w == 4 && Near(scene.lights[2].parameters.y, std::cos(0.5f)) &&
              Near(scene.lights[2].parameters.z, std::cos(0.2f)),
          "spot inner and outer cones map to cosines");
    std::string shearedNodes = nodes;
    Replace(shearedNodes, R"("translation":[1,2,3])", R"("translation":[1,2,3],"scale":[2,1,1])");
    Replace(shearedNodes, R"("name":"SunNode")", R"("name":"SunNode","rotation":[0,0.382683432,0,0.923879533])");
    const auto inherited = render::ImportGltf(fixture.Json(Document("{}", shearedNodes, extra)));
    const auto &frame = inherited.lights[0];
    Check(std::abs(frame.directionRange.x * frame.tangentWidth.x + frame.directionRange.y * frame.tangentWidth.y +
                   frame.directionRange.z * frame.tangentWidth.z) < 0.0001f,
          "scaled hierarchy yields an orthogonal punctual-light sampling frame");
}
void DefaultsAndMirroredUnindexedGeometry() {
    Fixture fixture;
    auto document = Document("{}", R"([{"mesh":0,"scale":[-1,1,1]}])");
    Replace(document, R"("POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"TANGENT":3)", R"("POSITION":0)");
    Replace(document, R"(,"indices":4,"material":0)", "");
    const auto scene = render::ImportGltf(fixture.Json(document));
    Check(scene.triangles.size() == 1 && scene.triangles[0].a.w == 1,
          "unindexed mesh uses dedicated standard default material");
    Check(Near(scene.triangles[0].a.x, -1) && Near(scene.triangles[0].b.x, 0) && Near(scene.triangles[0].b.y, 0),
          "mirrored transform reverses winding to preserve front faces");
    Check(Near(scene.attributes[0].normal0.z, 1) && Near(scene.attributes[0].uv01.x, 0),
          "missing normals and UV receive finite geometric defaults");
    Check(
        render::IsFinite({scene.attributes[0].tangent.x, scene.attributes[0].tangent.y, scene.attributes[0].tangent.z}),
        "missing UV tangent has a stable finite fallback");
}
void SparsePositionsAreApplied() {
    Fixture fixture;
    auto bytes = fixture.binary;
    bytes.resize(156);
    bytes[152] = 0;
    const float replacement[] = {2, 0, 0};
    const auto *first = reinterpret_cast<const std::uint8_t *>(replacement);
    bytes.insert(bytes.end(), first, first + sizeof(replacement));
    fixture.Write("mesh.bin", bytes);
    auto document = Document("{}", R"([{"mesh":0}])");
    Replace(document, R"("byteLength":150)", R"("byteLength":168)");
    Replace(
        document, R"("byteLength":6}],"accessors")",
        R"("byteLength":6},{"buffer":0,"byteOffset":152,"byteLength":1},{"buffer":0,"byteOffset":156,"byteLength":12}],"accessors")");
    Replace(
        document, R"("max":[1,1,0])",
        R"("max":[2,1,0],"sparse":{"count":1,"indices":{"bufferView":5,"componentType":5121},"values":{"bufferView":6}})");
    const auto scene = render::ImportGltf(fixture.Json(document));
    Check(Near(scene.triangles[0].a.x, 2), "sparse POSITION override is applied through cgltf unpacking");
}
void ExtensionTexturesArePacked() {
    Fixture fixture;
    SavePng(fixture.directory / "colors.png");
    auto document = Document(
        R"({"extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":1,"clearcoatTexture":{"index":0},"clearcoatRoughnessTexture":{"index":0}},"KHR_materials_specular":{"specularTexture":{"index":0},"specularColorTexture":{"index":0}}}})",
        R"([{"mesh":0}])", R"(,"images":[{"uri":"colors.png"}],"textures":[{"source":0}])");
    const auto scene = render::ImportGltf(fixture.Json(document));
    const auto &material = scene.materials[0];
    const auto &coat = scene.textureViews[material.textures2.y];
    const auto &specular = scene.textureViews[material.textures2.w];
    Check(scene.packedTexels[coat.image.x] == 0xffff00ffu && scene.packedTexels[coat.image.x + 1] == 0xffffff00u,
          "clearcoat factor R and roughness G are packed independently");
    Check(scene.packedTexels[specular.image.x] == 0xff0000ffu &&
              scene.packedTexels[specular.image.x + 1] == 0x8000ff00u,
          "specular color RGB and factor A retain both channels");
    Replace(document, R"("clearcoatRoughnessTexture":{"index":0})",
            R"("clearcoatRoughnessTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.25,0]}}})");
    Rejects([&] { render::ImportGltf(fixture.Json(document)); },
            "incompatible extension UV mappings must fail explicitly", "share UV transform");
}
void RejectsUnsupportedOrMalformedAssets() {
    Fixture fixture;
    auto required = Document();
    required.insert(1, R"("extensionsRequired":["KHR_draco_mesh_compression"],)");
    Rejects([&] { render::ImportGltf(fixture.Json(required)); }, "unsupported required extension must fail",
            "unsupported required extension");
    auto animated = Document();
    animated.insert(1, R"("animations":[{"channels":[],"samplers":[]}],)");
    Rejects([&] { render::ImportGltf(fixture.Json(animated)); },
            "animation must fail rather than silently use bind pose", "animated assets");
    auto morph = Document();
    Replace(morph, R"("indices":4,"material":0)", R"("indices":4,"material":0,"targets":[{"POSITION":0}])");
    Rejects([&] { render::ImportGltf(fixture.Json(morph)); }, "morph targets must fail explicitly", "morph target");
    auto topology = Document();
    Replace(topology, R"("indices":4,"material":0)", R"("indices":4,"material":0,"mode":5)");
    Rejects([&] { render::ImportGltf(fixture.Json(topology)); }, "non-triangle topology must fail explicitly",
            "triangle-list");
    auto indices = fixture.binary;
    indices[144] = 9;
    fixture.Write("mesh.bin", indices);
    Rejects([&] { render::ImportGltf(fixture.Json(Document())); }, "out of range indices must fail before access");
    fixture.Write("mesh.bin", fixture.binary);
    auto uv1 = Document(R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1}}})", R"([{"mesh":0}])",
                        R"(,"images":[{"uri":"colors.png"}],"textures":[{"source":0}])");
    SavePng(fixture.directory / "colors.png");
    Rejects([&] { render::ImportGltf(fixture.Json(uv1)); }, "unsupported texture coordinate set must fail explicitly");
    auto vertexColor = Document();
    Replace(vertexColor, R"("TANGENT":3)", R"("TANGENT":3,"COLOR_0":1)");
    Rejects([&] { render::ImportGltf(fixture.Json(vertexColor)); },
            "unsupported vertex colors must fail rather than disappear", "vertex color");
    Rejects([&] { render::ImportGltf(fixture.Json(Document(R"({"alphaMode":"BLEND"})"))); },
            "alpha blending cannot silently render as opaque", "alphaMode BLEND");
    Rejects([&] { render::ImportGltf(fixture.Json("{broken")); }, "malformed JSON must fail clearly");
    Rejects([&] { render::ImportGltf(fixture.directory / "missing.gltf"); }, "missing asset must fail clearly");
}
void WriteSmokeFixture(const std::filesystem::path &destination) {
    Fixture fixture;
    SavePng(fixture.directory / "colors.png");
    std::vector<float> positions, uvs;
    const auto quad = [&](std::array<render::Vec3, 4> vertices) {
        constexpr unsigned corners[] = {0, 1, 2, 0, 2, 3};
        constexpr float coordinates[][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        for (auto corner : corners) {
            const auto p = vertices[corner];
            positions.insert(positions.end(), {p.x, p.y, p.z});
            uvs.insert(uvs.end(), {coordinates[corner][0], coordinates[corner][1]});
        }
    };
    // A floor and closed box exercise UVs, multiple primitive materials, punctual shadows and silhouettes.
    quad({render::Vec3{-2, 0, -2}, {-2, 0, 2}, {2, 0, 2}, {2, 0, -2}});
    quad({render::Vec3{-0.5f, 0, -0.5f}, {-0.5f, 1.2f, -0.5f}, {0.5f, 1.2f, -0.5f}, {0.5f, 0, -0.5f}});
    quad({render::Vec3{0.5f, 0, 0.5f}, {0.5f, 1.2f, 0.5f}, {-0.5f, 1.2f, 0.5f}, {-0.5f, 0, 0.5f}});
    quad({render::Vec3{-0.5f, 0, 0.5f}, {-0.5f, 1.2f, 0.5f}, {-0.5f, 1.2f, -0.5f}, {-0.5f, 0, -0.5f}});
    quad({render::Vec3{0.5f, 0, -0.5f}, {0.5f, 1.2f, -0.5f}, {0.5f, 1.2f, 0.5f}, {0.5f, 0, 0.5f}});
    quad({render::Vec3{-0.5f, 1.2f, -0.5f}, {-0.5f, 1.2f, 0.5f}, {0.5f, 1.2f, 0.5f}, {0.5f, 1.2f, -0.5f}});
    quad({render::Vec3{-0.5f, 0, 0.5f}, {-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}});
    std::vector<std::uint8_t> bytes((positions.size() + uvs.size()) * sizeof(float));
    std::memcpy(bytes.data(), positions.data(), positions.size() * sizeof(float));
    std::memcpy(bytes.data() + positions.size() * sizeof(float), uvs.data(), uvs.size() * sizeof(float));
    // 生成器元数据与正式磁盘夹具一致，使品牌调整不改变解析和渲染对照数据。
    std::string document =
        R"({"asset":{"version":"2.0","generator":"EncoreEngine deterministic integration fixture"},"extensionsUsed":["KHR_lights_punctual","KHR_materials_clearcoat"],"buffers":[{"byteLength":840,"uri":"data:application/octet-stream;base64,)" +
        Base64(bytes) +
        R"("}],"bufferViews":[{"buffer":0,"byteLength":504},{"buffer":0,"byteOffset":504,"byteLength":336}],"accessors":[{"bufferView":0,"componentType":5126,"count":6,"type":"VEC3","min":[-2,0,-2],"max":[2,0,2]},{"bufferView":0,"byteOffset":72,"componentType":5126,"count":36,"type":"VEC3","min":[-0.5,0,-0.5],"max":[0.5,1.2,0.5]},{"bufferView":1,"componentType":5126,"count":6,"type":"VEC2"},{"bufferView":1,"byteOffset":48,"componentType":5126,"count":36,"type":"VEC2"}],"images":[{"uri":"data:image/png;base64,)" +
        Base64(Read(fixture.directory / "colors.png")) +
        R"("}],"textures":[{"source":0}],"materials":[{"name":"Textured Ground","pbrMetallicRoughness":{"baseColorFactor":[0.6,0.6,0.6,1],"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":0.85}},{"name":"Clearcoat Box","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0.1,"roughnessFactor":0.25},"extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":0.7,"clearcoatRoughnessFactor":0.2}}}],"meshes":[{"name":"GroundAndBox","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2},"material":0},{"attributes":{"POSITION":1,"TEXCOORD_0":3},"material":1}]}],"nodes":[{"name":"Fixture","mesh":0},{"name":"Warm Point","translation":[-1.5,2,-1.5],"extensions":{"KHR_lights_punctual":{"light":0}}},{"name":"Cool Spot","translation":[1.5,3,2],"rotation":[-0.4082179,0.2345697,0.1093817,0.8754261],"extensions":{"KHR_lights_punctual":{"light":1}}},{"name":"Sun","rotation":[-0.4226183,0,0,0.9063078],"extensions":{"KHR_lights_punctual":{"light":2}}}],"scenes":[{"nodes":[0,1,2,3]}],"scene":0,"extensions":{"KHR_lights_punctual":{"lights":[{"name":"Point","type":"point","color":[1,0.85,0.7],"intensity":80,"range":12},{"name":"Spot","type":"spot","color":[0.65,0.8,1],"intensity":100,"range":12,"spot":{"innerConeAngle":0.35,"outerConeAngle":0.65}},{"name":"Directional","type":"directional","intensity":0.25}]}}})";
    std::filesystem::create_directories(destination);
    std::ofstream file(destination / "textured_multilight.gltf", std::ios::binary);
    file << document;
    file.close();
    Check(bool(file), "integration fixture write");
    const auto scene = render::ImportGltf(destination / "textured_multilight.gltf");
    Check(scene.triangles.size() == 14 && scene.meshParts.size() == 2 && scene.lights.size() == 3,
          "published fixture imports its complete geometry and lights");
    std::ofstream invalid(destination / "unsupported_required.gltf", std::ios::binary);
    invalid << R"({"asset":{"version":"2.0"},"extensionsRequired":["KHR_draco_mesh_compression"]})";
}
} // namespace
int main(int argc, char **argv) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc == 3 && std::string(argv[1]) == "--write-fixtures") {
        try {
            WriteSmokeFixture(argv[2]);
            if (SUCCEEDED(apartment))
                CoUninitialize();
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            if (SUCCEEDED(apartment))
                CoUninitialize();
            return 1;
        }
    }
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"hierarchy and attributes", BakesHierarchyAndAttributes},
        {"material extensions", ConvertsMaterialExtensions},
        {"PNG and texture transform", ImportsTextureAndTransform},
        {"GLB and data URIs", ImportsGlbAndDataUris},
        {"whole model grouping", GroupsSceneFilesAsWholeModels},
        {"punctual lights", ImportsNamedPunctualLights},
        {"mirrored unindexed defaults", DefaultsAndMirroredUnindexedGeometry},
        {"sparse positions", SparsePositionsAreApplied},
        {"extension texture packing", ExtensionTexturesArePacked},
        {"malformed and unsupported", RejectsUnsupportedOrMalformedAssets}};
    unsigned failed = 0;
    for (const auto &[name, run] : tests) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    std::cout << tests.size() - failed << '/' << tests.size() << " asset import tests passed\n";
    return failed ? 1 : 0;
}
