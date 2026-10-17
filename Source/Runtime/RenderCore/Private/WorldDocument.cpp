#include "render/WorldDocument.h"
#include "PrimitiveFactory.h"
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/descriptor.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
// Windows 消息宏与 Protobuf 反射接口同名，不能让宏改写正式方法调用。
#undef GetMessage
#endif

namespace render {
namespace {
constexpr std::size_t maximumDocumentBytes = 64u * 1024u * 1024u;
constexpr std::uint64_t maximumTexturePixels = 64u * 1024u * 1024u;
constexpr std::uint64_t maximumWorldTexturePixels = 256u * 1024u * 1024u;
Float4 Lane(const config::Vec4 &v) {
    return {v.x(), v.y(), v.z(), v.w()};
}
void Lane(const Float4 &v, config::Vec4 *out) {
    out->set_x(v.x);
    out->set_y(v.y);
    out->set_z(v.z);
    out->set_w(v.w);
}
void Check(bool condition, const std::string &message) {
    if (!condition)
        throw std::invalid_argument(message);
}
bool Utf8(const std::string &text) {
    // 严格拒绝过长编码、代理区和截断序列，避免路径在不同系统边界产生歧义。
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            continue;
        unsigned count = 0, value = 0, minimum = 0;
        if ((c & 0xe0) == 0xc0) {
            count = 1;
            value = c & 31;
            minimum = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            count = 2;
            value = c & 15;
            minimum = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            count = 3;
            value = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i)
            return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
void ValidatePath(const std::filesystem::path &path) {
    // 文件系统边界独立校验，避免嵌入空字符被原生文件 API 截断。
    const auto utf8 = path.u8string();
    const std::string text(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    Check(!text.empty() && text.size() < 32760 && text.find('\0') == std::string::npos && Utf8(text),
          "世界文档路径无效");
}
std::string Utf8Path(const std::filesystem::path& path) {
    // 持久化引用使用通用分隔符，避免 Windows 原生反斜杠进入可移植文档。
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool BinaryWorldPath(const std::filesystem::path& path) {
    // Windows 文件筛选与浏览器忽略扩展名大小写，编码选择必须遵循相同规则。
    auto extension = path.extension().u8string();
    for (auto& letter : extension) if (letter >= u8'A' && letter <= u8'Z') letter += u8'a' - u8'A';
    return extension == u8".pb";
}
std::filesystem::path PayloadPath(const std::filesystem::path& document, const std::string& relative) {
    // 读取时相对于文档解析；保存时允许已加载的绝对位置重新计算可移植引用。
    const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(relative.data()), relative.size()));
    return std::filesystem::absolute(path.is_absolute() ? path : document.parent_path() / path).lexically_normal();
}
std::uint32_t LittleEndian(std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::little) return value;
    else return (value >> 24) | ((value >> 8) & 0xff00u) | ((value << 8) & 0xff0000u) | (value << 24);
}
void ReadPayload(config::TextureAsset& texture, const std::filesystem::path& path, bool compare) {
    // 在任何大分配之前验证乘法、整幅图像预算和精确字节数，截断与附加数据都必须拒绝。
    const auto count = std::uint64_t(texture.width()) * texture.height();
    Check(texture.width() && texture.height() && texture.width() <= 16384 && texture.height() <= 16384 &&
        count <= maximumTexturePixels, "外部纹理尺寸超过容量");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Check(bool(input) && input.tellg() >= 0 && static_cast<std::uint64_t>(input.tellg()) == count * 4,
        "外部纹理不可读或 RGBA 字节数与尺寸不一致：" + Utf8Path(path));
    input.seekg(0);
    if (!compare) texture.mutable_rgba()->Resize(static_cast<int>(count), 0);
    std::array<std::uint32_t, 16384> chunk{};
    for (std::uint64_t first = 0; first < count;) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), count - first));
        Check(bool(input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(size * 4))),
            "外部纹理读取不完整");
        for (std::size_t i = 0; i < size; ++i) {
            const auto pixel = LittleEndian(chunk[i]);
            if (compare && pixel != texture.rgba(static_cast<int>(first + i)))
                throw std::invalid_argument("外部纹理已变化，请重新导入后保存");
            if (!compare) texture.set_rgba(static_cast<int>(first + i), pixel);
        }
        first += size;
    }
}
void HydrateTextures(WorldDocument& document, const std::filesystem::path& path) {
    // 外置载荷只在文件边界补齐，后续 ECS 与 GPU 继续消费同一完整的内存图像契约。
    std::uint64_t pixels{};
    for (auto& texture : *document.mutable_textures()) {
        const auto count = std::uint64_t(texture.width()) * texture.height();
        Check(count <= maximumTexturePixels && pixels <= maximumWorldTexturePixels - count, "世界纹理超过一 GiB 容量");
        pixels += count;
        if (texture.rgba_path().empty()) continue;
        Check(texture.rgba_size() == 0, "外部纹理不能同时携带内嵌像素");
        const auto& reference = texture.rgba_path();
        const std::filesystem::path relative(std::u8string(reinterpret_cast<const char8_t*>(reference.data()), reference.size()));
        Check(!relative.has_root_path(), "已保存的外部纹理必须使用相对路径");
        const auto resolved = PayloadPath(path, texture.rgba_path());
        ReadPayload(texture, resolved, false);
        texture.set_rgba_path(Utf8Path(resolved));
    }
}
void ValidateFields(const google::protobuf::Message &message) {
    // 官方反射遍历所有已写入字段，使新增配置分量同样受有限值与编码约束。
    const auto *reflection = message.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor *> fields;
    reflection->ListFields(message, &fields);
    for (const auto *field : fields) {
        // 整数像素不存在非有限值，跳过整幅固定宽度图像可避免逐纹素反射开销。
        using F = google::protobuf::FieldDescriptor;
        if (field->cpp_type() != F::CPPTYPE_FLOAT && field->cpp_type() != F::CPPTYPE_STRING &&
            field->cpp_type() != F::CPPTYPE_MESSAGE) continue;
        const int count = field->is_repeated() ? reflection->FieldSize(message, field) : 1;
        for (int i = 0; i < count; ++i) {
            if (field->cpp_type() == F::CPPTYPE_FLOAT) {
                const auto value = field->is_repeated() ? reflection->GetRepeatedFloat(message, field, i)
                                                        : reflection->GetFloat(message, field);
                Check(std::isfinite(value), "文档包含非有限数值：" + field->full_name());
            } else if (field->cpp_type() == F::CPPTYPE_STRING) {
                const auto value = field->is_repeated() ? reflection->GetRepeatedString(message, field, i)
                                                        : reflection->GetString(message, field);
                Check(Utf8(value) && value.find('\0') == std::string::npos,
                      "文档字符串包含无效 UTF-8 或空字符：" + field->full_name());
                if (field->name().find("path") != std::string::npos)
                    Check(value.size() < 32760 && value.find_first_of("\r\n<>\"|?*") == std::string::npos,
                          "来源路径长度或字符无效");
                if (field->name().find("source") != std::string::npos &&
                    field->name().find("path") == std::string::npos)
                    Check(value.size() <= 1024u * 1024u, "脚本源码超过一 MiB 限制");
            } else if (field->cpp_type() == F::CPPTYPE_MESSAGE)
                ValidateFields(field->is_repeated() ? reflection->GetRepeatedMessage(message, field, i)
                                                    : reflection->GetMessage(message, field));
        }
    }
}
template <class Collection> auto Index(const Collection &items, const char *type) {
    using Item = typename Collection::value_type;
    std::map<std::string, const Item *> result;
    for (const auto &item : items)
        Check(!item.id().empty() && result.emplace(item.id(), &item).second,
              std::string(type) + "标识为空或重复：" + item.id());
    return result;
}
class ParseErrors final : public google::protobuf::io::ErrorCollector {
  public:
    void AddError(int line, int column, const std::string &message) override {
        text += "第" + std::to_string(line + 1) + "行第" + std::to_string(column + 1) + "列：" + message + "\n";
    }
    void AddWarning(int, int, const std::string &) override {}
    std::string text; // 官方文本解析器返回的具体行列诊断。
};
std::uint32_t &TextureSlot(GpuMaterial &m, unsigned slot) {
    auto &lane = slot < 4 ? m.textures : m.textures2;
    switch (slot % 4) {
    case 0:
        return lane.x;
    case 1:
        return lane.y;
    case 2:
        return lane.z;
    default:
        return lane.w;
    }
}
} // 匿名命名空间

Vec3 ReadVector(const config::Vec3 &v) {
    return {v.x(), v.y(), v.z()};
}
void WriteVector(const Vec3 &v, config::Vec3 &out) {
    out.set_x(v.x);
    out.set_y(v.y);
    out.set_z(v.z);
}
GpuMaterial ReadMaterialParameters(const config::MaterialParameters &p) {
    GpuMaterial m;
    // 缺失字段使用引擎默认值，作者只需填写需要覆盖的参数组。
    m.baseRoughness = {0.73f, 0.73f, 0.73f, 0.78f};
    m.transmissionIor = {0, 1.5f, 0, 0};
    if (p.has_base_roughness())
        m.baseRoughness = Lane(p.base_roughness());
    if (p.has_emission_metallic())
        m.emissionMetallic = Lane(p.emission_metallic());
    if (p.has_transmission_ior())
        m.transmissionIor = Lane(p.transmission_ior());
    if (p.has_principled())
        m.principled = Lane(p.principled());
    if (p.has_sheen())
        m.sheen = Lane(p.sheen());
    if (p.has_medium())
        m.medium = Lane(p.medium());
    if (p.has_extras())
        m.extras = Lane(p.extras());
    if (p.has_tangent_frame())
        m.tangentFrame = Lane(p.tangent_frame());
    m.flags = {p.thin() ? 1u : 0u, p.double_sided() ? 1u : 0u, p.alpha_mode(), 0};
    return m;
}
void WriteMaterialParameters(const GpuMaterial &m, config::MaterialParameters &p) {
    // 不把运行时纹理槽或介质内部状态保存为作者关联。
    Lane(m.baseRoughness, p.mutable_base_roughness());
    Lane(m.emissionMetallic, p.mutable_emission_metallic());
    Lane(m.transmissionIor, p.mutable_transmission_ior());
    Lane(m.principled, p.mutable_principled());
    Lane(m.sheen, p.mutable_sheen());
    Lane(m.medium, p.mutable_medium());
    Lane(m.extras, p.mutable_extras());
    Lane(m.tangentFrame, p.mutable_tangent_frame());
    p.set_thin(m.flags.x != 0);
    p.set_double_sided(m.flags.y != 0);
    p.set_alpha_mode(m.flags.z);
}
const config::Scene &FindDocumentScene(const WorldDocument &document, std::string_view id) {
    const auto selected = id.empty() ? std::string_view(document.default_scene_id()) : id;
    for (const auto &level : document.levels())
        for (const auto &scene : level.scenes())
            if (scene.id() == selected)
                return scene;
    throw std::invalid_argument("找不到场景：" + std::string(selected));
}

void ValidateWorldDocument(const WorldDocument &d) {
    Check(d.format_version() == 1 && !d.id().empty(), "世界标识为空或文档格式版本不受支持");
    ValidateFields(d);
    // 导入后尚未外置的内存像素也有独立预算，文件边界再检查真正编码后的配置长度。
    auto metadataBytes = d.ByteSizeLong();
    std::uint64_t texturePixels{};
    for (const auto& texture : d.textures()) {
        const auto count = std::uint64_t(texture.width()) * texture.height();
        Check(count <= maximumTexturePixels && texturePixels <= maximumWorldTexturePixels - count,
            "世界纹理超过一 GiB 容量");
        texturePixels += count;
        metadataBytes -= static_cast<std::size_t>(texture.rgba_size()) * 4;
    }
    Check(metadataBytes <= maximumDocumentBytes, "世界文档超过六十四 MiB 限制");
    const auto materials = Index(d.materials(), "材质");
    const auto textures = Index(d.textures(), "纹理");
    const auto meshes = Index(d.meshes(), "网格");
    (void)Index(d.levels(), "关卡");
    for (const auto &material : d.materials())
        Check(material.parameters().alpha_mode() <= 1, "材质透明模式无效");
    for (const auto &texture : d.textures())
        Check(texture.width() && texture.height() &&
                  std::uint64_t(texture.width()) * texture.height() == static_cast<std::uint64_t>(texture.rgba_size()),
              "纹理像素数量与尺寸不一致");
    // 在任何 ECS 或 GPU 资源替换之前验证全部场景，包括当前未激活的场景。
    std::set<std::string> scenes, objects;
    for (const auto &mesh : d.meshes()) {
        auto geometry = BuildPrimitive(mesh);
        Check(!geometry.triangles.empty() && geometry.triangles.size() < (1u << 24) &&
                  geometry.attributes.size() < (1u << 24),
              "网格为空或超过精确索引范围");
        if (mesh.has_box()) {
            const auto s = ReadVector(mesh.box().size());
            Check(s.x > 0 && s.y > 0 && s.z > 0, "盒体尺寸必须为正数");
        }
        for (const auto &t : geometry.triangles) {
            const Vec3 a{t.a.x, t.a.y, t.a.z}, b{t.b.x, t.b.y, t.b.z}, c{t.c.x, t.c.y, t.c.z};
            const auto area = Length(Cross(b - a, c - a));
            Check(IsFinite(a) && IsFinite(b) && IsFinite(c) && std::isfinite(area) && area > 0,
                  "网格包含退化或无效三角形");
            Check(t.b.w >= 0 && t.b.w <= static_cast<float>(geometry.attributes.size()), "网格属性引用越界");
        }
    }
    for (const auto &level : d.levels())
        for (const auto &scene : level.scenes()) {
            Check(!scene.id().empty() && scenes.insert(scene.id()).second, "场景标识为空或重复");
            unsigned lights = 0;
            std::set<std::string> emitterOwners;
            for (const auto &o : scene.objects()) {
                Check(!o.id().empty() && objects.insert(o.id()).second, "对象标识为空或重复：" + o.id());
                const auto &s = o.transform().scale();
                Check((!s.has_x() || s.x() != 0) && (!s.has_y() || s.y() != 0) && (!s.has_z() || s.z() != 0),
                      "对象缩放不可为零");
                if (!o.mesh_id().empty()) {
                    Check(meshes.contains(o.mesh_id()) && materials.contains(o.material_id()),
                          "对象缺少有效网格或材质引用：" + o.id());
                    auto geometry = BuildPrimitive(*meshes.at(o.mesh_id()));
                    const auto first = o.first_triangle(), count = o.triangle_count();
                    Check(first < geometry.triangles.size() && (!count || count <= geometry.triangles.size() - first),
                          "对象网格范围越界");
                    // 表面使用对象局部范围，必须无空洞、无重叠地覆盖完整模型。
                    std::size_t surfaceEnd = 0;
                    const auto objectCount = count ? count : geometry.triangles.size() - first;
                    for (const auto& surface : o.surface_materials()) {
                        Check(materials.contains(surface.material_id()) && surface.first_triangle() == surfaceEnd &&
                                  surface.triangle_count() > 0 && surface.triangle_count() <= objectCount - surfaceEnd,
                              "模型表面范围或材质引用无效");
                        surfaceEnd += surface.triangle_count();
                    }
                    Check(o.surface_materials().empty() || surfaceEnd == objectCount, "模型表面没有覆盖完整几何");
                    ApplyDocumentTransform(geometry, o.transform());
                } else
                    Check(o.material_id().empty() && o.texture_bindings().empty() && o.surface_materials().empty(),
                          "无网格对象不能绑定表面资源");
                // 模型各表面与旧单材质对象经过同一纹理引用校验。
                const auto validateBindings = [&](const auto& owner) {
                    std::set<unsigned> slots;
                    for (const auto &binding : owner.texture_bindings()) {
                        Check(binding.slot() < 8 && slots.insert(binding.slot()).second && textures.contains(binding.texture_id()),
                              "对象纹理槽重复、越界或引用不存在");
                        Check(binding.wrap_u() <= 2 && binding.wrap_v() <= 2 && binding.rotation().z() == 0,
                              "纹理采样方式或坐标集无效");
                    }
                };
                validateBindings(o);
                for (const auto& surface : o.surface_materials()) validateBindings(surface);
                if (o.has_camera()) {
                    const auto &c = o.camera();
                    Check(c.vertical_fov_degrees() > 0 && c.vertical_fov_degrees() < 180 && c.near_plane() > 0 &&
                              c.far_plane() > c.near_plane(),
                          "摄像机投影参数无效");
                }
                if (o.has_light()) {
                    const auto &l = o.light();
                    if (l.enabled())
                        ++lights;
                    Check(l.intensity() >= 0 && l.range() >= 0 && l.source_radius() >= 0 && l.width() > 0 &&
                              l.height() > 0 && l.color().x() >= 0 && l.color().y() >= 0 && l.color().z() >= 0,
                          "光源颜色、能量或尺寸无效");
                    Check(l.inner_angle_degrees() >= 0 && l.outer_angle_degrees() > 0 &&
                              l.outer_angle_degrees() <= 90 && l.inner_angle_degrees() <= l.outer_angle_degrees(),
                          "光源锥角无效");
                    const auto direction = l.has_direction() ? Normalize(ReadVector(l.direction())) : Vec3{0, -1, 0};
                    const auto tangent = l.has_tangent() ? Normalize(ReadVector(l.tangent())) : Vec3{1, 0, 0};
                    const auto bitangent = l.has_bitangent() ? Normalize(ReadVector(l.bitangent())) : Vec3{0, 0, 1};
                    Check(Length(direction) > .9f && Length(tangent) > .9f && Length(bitangent) > .9f &&
                              std::abs(Dot(tangent, bitangent)) < .001f &&
                              Dot(Cross(tangent, bitangent), direction) > .999f,
                          "光源坐标轴必须构成正交发射坐标系");
                    if (!l.emitter_object_id().empty()) {
                        Check((l.type() == config::Light::RECTANGLE || l.type() == config::Light::DISK) &&
                                  emitterOwners.insert(l.emitter_object_id()).second,
                              "发光体只能由一个面光源拥有");
                        const auto found =
                            std::find_if(scene.objects().begin(), scene.objects().end(), [&](const auto &x) {
                                return x.id() == l.emitter_object_id() && !x.mesh_id().empty();
                            });
                        Check(found != scene.objects().end(), "光源发光体对象引用不存在或没有网格");
                    }
                }
            }
            Check(lights <= 16, "一个场景最多允许十六个启用光源");
        }
    Check(!scenes.empty() && scenes.contains(d.default_scene_id()), "世界没有有效默认场景");
}

WorldDocument LoadWorldDocument(const std::filesystem::path &path) {
    ValidatePath(path);
    // 局部消息完成解析与所有引用校验后才交付，失败不会改变当前世界。
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("无法打开世界文档");
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximumDocumentBytes)
        throw std::length_error("世界文档大小无效");
    std::string bytes(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("世界文档读取不完整");
    WorldDocument document;
    if (BinaryWorldPath(path)) {
        if (!document.ParseFromString(bytes))
            throw std::runtime_error("Protobuf 二进制世界文档解析失败");
    } else {
        // 接受常见编辑器写入的 UTF-8 BOM，实际语法仍全部交给官方解析器。
        if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
        ParseErrors errors;
        google::protobuf::TextFormat::Parser parser;
        parser.RecordErrorsTo(&errors);
        if (!parser.ParseFromString(bytes, &document))
            throw std::runtime_error("Protobuf 文本世界文档解析失败：" + errors.text);
    }
    // 文件路径在第一次打开 sidecar 前也必须经过严格 UTF-8 和空字符校验。
    ValidateFields(document);
    HydrateTextures(document, path);
    ValidateWorldDocument(document);
    return document;
}
void SaveWorldDocument(const WorldDocument &document, const std::filesystem::path &path) {
    ValidatePath(path);
    ValidateWorldDocument(document);
    auto stored = document;
    // 保存只剥离已逐字核对的外部载荷；另存为时重算相对位置，不静默指向错误图像。
    for (auto& texture : *stored.mutable_textures()) {
        if (texture.rgba_path().empty()) continue;
        const auto resolved = PayloadPath(path, texture.rgba_path());
        ReadPayload(texture, resolved, true);
        texture.set_rgba_path(Utf8Path(std::filesystem::relative(resolved, std::filesystem::absolute(path).parent_path())));
        texture.clear_rgba();
    }
    Check(stored.ByteSizeLong() <= maximumDocumentBytes, "世界文档超过六十四 MiB 限制");
    std::string bytes;
    if (BinaryWorldPath(path)) {
        if (!stored.SerializeToString(&bytes))
            throw std::runtime_error("世界文档二进制编码失败");
    } else {
        google::protobuf::TextFormat::Printer printer;
        printer.SetUseUtf8StringEscaping(true);
        if (!printer.PrintToString(stored, &bytes)) throw std::runtime_error("世界文档文本编码失败");
    }
    Check(bytes.size() <= maximumDocumentBytes, "世界文档编码后超过六十四 MiB 限制，请改用 .pb");
    // 同目录临时文件写入完成后原子替换，失败不会留下半写入的正式配置。
    static std::atomic_uint64_t serial{0};
    auto temporary = path;
    temporary += ".tmp." + std::to_string(++serial);
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !output.flush())
                throw std::runtime_error("世界文档写入失败");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("世界文档原子替换失败");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

Scene BakeScene(const WorldDocument &document, std::string_view sceneId, bool applyTransforms) {
    ValidateWorldDocument(document);
    const auto &authored = FindDocumentScene(document, sceneId);
    Scene result;
    const auto meshes = Index(document.meshes(), "网格");
    const auto textures = Index(document.textures(), "纹理");
    std::map<std::string, std::uint32_t> materialIndices;
    for (const auto &material : document.materials()) {
        materialIndices.emplace(material.id(), static_cast<std::uint32_t>(result.materials.size()));
        result.materials.push_back(ReadMaterialParameters(material.parameters()));
        result.materialNames.push_back(material.name());
        result.materialIds.push_back(material.id());
        result.materialObjectIds.emplace_back();
    }
    std::map<std::string, std::uint32_t> textureOffsets;
    // 模型内部各表面独立解析材质与贴图，仍归属于同一个作者对象。
    const auto resolveMaterial = [&](const auto& binding, const config::Object& object) {
        auto material = materialIndices.at(binding.material_id());
        if (binding.has_material_override() || !binding.texture_bindings().empty() || binding.has_material_script_source() ||
            binding.has_material_script_path()) {
            config::MaterialParameters resolved;
            WriteMaterialParameters(result.materials[material], resolved);
            if (binding.has_material_override())
                resolved.MergeFrom(binding.material_override());
            auto parameters = ReadMaterialParameters(resolved);
            for (const auto &textureBinding : binding.texture_bindings()) {
                const auto &texture = *textures.at(textureBinding.texture_id());
                auto [found, inserted] =
                    textureOffsets.emplace(texture.id(), static_cast<std::uint32_t>(result.packedTexels.size()));
                if (inserted)
                    result.packedTexels.insert(result.packedTexels.end(), texture.rgba().begin(), texture.rgba().end());
                GpuTextureView view;
                view.image = {found->second, texture.width(), texture.height(),
                              textureBinding.wrap_u() | (textureBinding.wrap_v() << 2)};
                if (textureBinding.has_offset_scale())
                    view.offsetScale = Lane(textureBinding.offset_scale());
                if (textureBinding.has_rotation())
                    view.rotation = Lane(textureBinding.rotation());
                TextureSlot(parameters, textureBinding.slot()) = static_cast<std::uint32_t>(result.textureViews.size());
                result.textureViews.push_back(view);
                result.textureIds.push_back(textureBinding.texture_id());
            }
            material = static_cast<std::uint32_t>(result.materials.size());
            result.materials.push_back(parameters);
            result.materialNames.push_back(result.materialNames[materialIndices.at(binding.material_id())] + " / " + object.name());
            result.materialIds.push_back(binding.material_id());
            result.materialObjectIds.push_back(object.id());
        }
        return material;
    };
    for (const auto &o : authored.objects()) {
        if (o.mesh_id().empty()) continue;
        auto geometry = BuildPrimitive(*meshes.at(o.mesh_id()));
        if (applyTransforms) ApplyDocumentTransform(geometry, o.transform());
        const auto first = o.first_triangle(),
                   count = o.triangle_count() ? o.triangle_count()
                                              : static_cast<std::uint32_t>(geometry.triangles.size()) - first;
        const bool useSurfaces = !o.surface_materials().empty() && o.use_surface_materials();
        auto material = useSurfaces ? materialIndices.at(o.material_id()) : resolveMaterial(o, o);
        std::vector<std::uint32_t> surfaceMaterials;
        for (const auto& surface : o.surface_materials()) {
            const auto slot = resolveMaterial(surface, o);
            surfaceMaterials.insert(surfaceMaterials.end(), surface.triangle_count(), slot);
        }
        if (useSurfaces) material = surfaceMaterials.front();
        // 运行时源资源保留原始表面；直接烘焙的渲染场景则执行当前实例的整体覆盖。
        const bool preserveSurfaces = !surfaceMaterials.empty() && (useSurfaces || !applyTransforms);
        const auto attributeOffset = result.attributes.size();
        result.attributes.insert(result.attributes.end(), geometry.attributes.begin(), geometry.attributes.end());
        result.meshParts.push_back(
            {o.name(), static_cast<std::uint32_t>(result.triangles.size()), count, material, o.id(), preserveSurfaces});
        for (std::size_t i = first; i < std::size_t(first) + count; ++i) {
            auto t = geometry.triangles[i];
            t.a.w = static_cast<float>(preserveSurfaces ? surfaceMaterials[i - first] : material);
            if (t.b.w > 0)
                t.b.w += static_cast<float>(attributeOffset);
            result.triangles.push_back(t);
        }
    }
    // 解析光源与发光三角形使用同一稳定标签，独立 Bake 也保留完整照明语义。
    for (const auto &object : authored.objects()) {
        if (object.has_camera()) {
            const auto& camera = object.camera();
            result.cameras.push_back({object.name(), ReadVector(object.transform().position()),
                ReadVector(object.transform().rotation_degrees()), camera.vertical_fov_degrees(),
                camera.near_plane(), camera.far_plane(), camera.active()});
        }
        if (!object.has_light() || !object.light().enabled())
            continue;
        const auto &source = object.light();
        const auto pose = ReadVector(object.transform().position());
        const auto &euler = object.transform().rotation_degrees();
        const auto basis = MakeCameraBasis(euler.x(), euler.y(), euler.z());
        const auto rotate = [&](Vec3 p) { return Normalize(basis.right * p.x + basis.up * p.y + basis.forward * p.z); };
        const auto direction = rotate(source.has_direction() ? ReadVector(source.direction()) : Vec3{0, -1, 0});
        const auto tangent = rotate(source.has_tangent() ? ReadVector(source.tangent()) : Vec3{1, 0, 0});
        const auto bitangent = rotate(source.has_bitangent() ? ReadVector(source.bitangent()) : Vec3{0, 0, 1});
        const auto color = source.has_color() ? ReadVector(source.color()) : Vec3{1, 1, 1};
        GpuLight light;
        light.positionType = {pose.x, pose.y, pose.z, static_cast<float>(source.type())};
        light.directionRange = {direction.x, direction.y, direction.z, source.range()};
        light.colorIntensity = {color.x, color.y, color.z, source.intensity()};
        light.tangentWidth = {tangent.x, tangent.y, tangent.z, source.width()};
        light.bitangentHeight = {bitangent.x, bitangent.y, bitangent.z, source.height()};
        constexpr float radians = 3.141592654f / 180;
        light.parameters = {source.source_radius(), std::cos(source.outer_angle_degrees() * radians),
                            std::cos(source.inner_angle_degrees() * radians), 0};
        if (!source.emitter_object_id().empty())
            for (const auto &part : result.meshParts)
                if (part.objectId == source.emitter_object_id()) {
                    light.shadow.z = part.materialIndex;
                    for (std::size_t i = part.firstTriangle; i < std::size_t(part.firstTriangle) + part.triangleCount;
                         ++i)
                        result.triangles[i].c.w = static_cast<float>(result.lights.size() + 1);
                }
        result.lights.push_back(light);
        result.lightNames.push_back(object.name());
    }
    return result;
}
std::filesystem::path DefaultWorldPath() {
    // 优先使用发布内容，开发环境再回退到构建系统注入的源目录。
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (size && size < executable.size()) {
        executable.resize(size);
        const auto path =
            std::filesystem::path(executable).parent_path() / L"Content" / L"Worlds" / L"default.textproto";
        if (std::filesystem::exists(path))
            return path;
    }
#endif
#ifdef RENDER_WORLD_DIRECTORY
    return std::filesystem::path(RENDER_WORLD_DIRECTORY) / L"default.textproto";
#else
    return std::filesystem::path("Content") / "Worlds" / "default.textproto";
#endif
}
} // 渲染命名空间
