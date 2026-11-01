#include "texture/texture.h"
#include "platform/platform.h"
#include "render/gl.h"
#include "ui/widgets.h"
#include "BaseUtils/BUPng.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace
{
    bool IsDds(const std::vector<uint8_t>& bytes)
    {
        return bytes.size() >= 4 && BUDds::ReadU32LE(bytes.data()) == BUDds::FourCC('D', 'D', 'S', ' ');
    }

    std::vector<uint8_t> ResizeNearest(const BUPng::Image& image, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const size_t sourceRow = size_t(y) * image.height / height * image.width;
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t source = (sourceRow + size_t(x) * image.width / width) * 4;
                std::copy_n(image.rgba.data() + source, 4, rgba.data() + (size_t(y) * width + x) * 4);
            }
        }
        return rgba;
    }

    void DrawCheckerboard(const ImVec2& origin, const ImVec2& size)
    {
        constexpr float tile = 16;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (int row = 0; row * tile < size.y; ++row)
            for (int column = 0; column * tile < size.x; ++column)
            {
                const ImVec2 min(origin.x + column * tile, origin.y + row * tile);
                const ImVec2 max(origin.x + std::min((column + 1) * tile, size.x), origin.y + std::min((row + 1) * tile, size.y));
                drawList->AddRectFilled(min, max, (row + column) % 2 ? IM_COL32(215, 215, 215, 255) : IM_COL32(240, 240, 240, 255));
            }
    }
}

CMChunkResourceHeader SelectedTextureHeader(const PackageSession& session)
{
    const auto header = session.SelectedResource();
    if (!header || header->GetResourceType() != kCMResourceType_Texture)
        throw std::runtime_error("Select a texture first.");
    return *header;
}

RZTexture SelectedTexture(const PackageSession& session)
{
    return RZTexture::Load(*session.package, SelectedTextureHeader(session));
}

BUPng::Image DecodeTextureImage(const std::vector<uint8_t>& file)
{
    if (IsDds(file))
    {
        const auto image = BUDds::Decode(file);
        if (image.volume || image.cubemap)
            throw std::runtime_error("Use texture replacement to import a volume or cubemap DDS.");
        return {image.width, image.height, BUDds::DecodeRGBA8(image)};
    }
    return BUPng::Decode(file);
}

void ExtractTexture(const PKPackage& package, const CMChunkResourceHeader& header, const std::string& path)
{
    const RZTexture texture = RZTexture::Load(package, header);
    auto extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
    if (extension == ".dds")
        WriteFile(std::filesystem::u8path(path), texture.ToDds());
    else
    {
        const auto rgba = texture.DecodeLevel0();
        WriteFile(std::filesystem::u8path(path), BUPng::EncodeRGBA8(rgba.data(), texture.PreviewWidth(), texture.desc.height));
    }
}

void ReplaceTexture(PackageSession& session, const CMChunkResourceHeader& header, const std::string& path,
    uint32_t width, uint32_t height)
{
    if (!width || !height || width > kMaxTextureSize || height > kMaxTextureSize)
        throw std::runtime_error("Width/height must be between 1 and 4096.");
    if (!session.package || header.GetResourceType() != kCMResourceType_Texture)
        throw std::runtime_error("Select a texture first.");

    const auto bytes = ReadFile(std::filesystem::u8path(path));
    RZTexture texture = RZTexture::Load(*session.package, header);
    if (IsDds(bytes))
    {
        if (width != texture.PreviewWidth() || height != texture.desc.height)
            throw std::runtime_error("DDS replacement must keep the texture dimensions.");
        texture.ReplaceDds(bytes);
    }
    else
    {
        const auto image = DecodeTextureImage(bytes);
        const auto rgba = ResizeNearest(image, width, height);
        texture.Reencode(rgba.data(), width, height);
    }
    texture.Store(*session.package);
    session.MarkEdited("Texture replaced (original format kept). Save the package to write to disk.");
}

TexturePreview::~TexturePreview()
{
    Clear();
}

void TexturePreview::Update(const PackageSession& session)
{
    const SelectionKey key = SelectionKey::Of(session);
    if (key == shown)
        return;
    Clear();
    shown = key;
    if (!key.package || !key.chunk)
        return;
    try
    {
        const auto resource = session.SelectedResource();
        if (!resource || resource->GetResourceType() != kCMResourceType_Texture)
            return;
        active = true;
        name = resource->GetName();
        const RZTexture decoded = RZTexture::Load(*key.package, *resource);
        descriptor = decoded.desc;
        Upload(decoded);
    }
    catch (const std::exception& ex)
    {
        active = true;
        error = ex.what();
    }
}

void TexturePreview::Upload(const RZTexture& decoded)
{
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    const GLint width = GLint(descriptor.PreviewWidth()), height = GLint(descriptor.height);
    if (!width || !height || width > maximum || height > maximum)
        throw std::runtime_error("Texture dimensions exceed the renderer's limits.");
    const auto rgba = decoded.DecodeLevel0();
    if (rgba.size() != size_t(width) * height * 4)
        throw std::runtime_error("Texture decoder returned an unexpected pixel count.");
    texture = CreateRgbaTexture(rgba.data(), width, height, GL_CLAMP_TO_EDGE);
}

void TexturePreview::Clear()
{
    if (texture)
        glDeleteTextures(1, &texture);
    texture = 0;
    shown = {};
    active = false;
    descriptor = {};
    name.clear();
    error.clear();
}

void TexturePreview::Draw() const
{
    ImGui::TextWrapped("%s", name.c_str());
    if (!error.empty())
    {
        ImGui::TextWrapped("Preview unavailable: %s", error.c_str());
        return;
    }
    if (descriptor.IsVolume())
        ImGui::TextDisabled("%u x %u x %u (slices left to right)", descriptor.width, descriptor.height, descriptor.depth);
    else if (descriptor.IsPC() && descriptor.IsCube())
        ImGui::TextDisabled("%u x %u, six faces: +X -X +Y -Y +Z -Z", descriptor.width, descriptor.height);
    else
        ImGui::TextDisabled("%u x %u", descriptor.width, descriptor.height);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = std::min({1.0f, available.x / descriptor.PreviewWidth(), available.y / descriptor.height});
    if (scale <= 0)
        return;
    const ImVec2 size(descriptor.PreviewWidth() * scale, descriptor.height * scale);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (available.x - size.x) / 2, cursor.y + (available.y - size.y) / 2));
    DrawCheckerboard(ImGui::GetCursorScreenPos(), size);
    ImGui::Image(static_cast<ImTextureID>(texture), size);
}

void TexturePreview::DrawFields() const
{
    if (!active || !error.empty())
        return;
    FieldRow("Texture", "string", name);
    FieldRow("Width", "u32", std::to_string(descriptor.width));
    FieldRow("Height", "u32", std::to_string(descriptor.height));
    FieldRow("kind", "u32", std::to_string(descriptor.kind));
    FieldRow("flags", "u32", HexString(descriptor.flags));
    FieldRow("mipLevels", "u32", std::to_string(descriptor.mipLevels));
    FieldRow("depth", "u32", std::to_string(descriptor.depth));
    if (descriptor.IsPC())
    {
        FieldRow("Storage", "", "PC DDS");
        FieldRow("Format", "",
                 descriptor.d3dFormat == BUDds::kDXT1   ? "BC1 (DXT1)"
                 : descriptor.d3dFormat == BUDds::kDXT3 ? "BC2 (DXT3)"
                 : descriptor.d3dFormat == BUDds::kDXT5 ? "BC3 (DXT5)"
                                                        : "RGB / alpha");
    }
    else
        FieldRow("d3dFormat", "u32", HexString(descriptor.d3dFormat));
}
