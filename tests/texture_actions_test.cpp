#include "texture/texture.h"
#include "platform/platform.h"
#include "BaseUtils/BUPng.h"
#include <iostream>

namespace
{
    void Check(bool ok, const char* message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }

    template<typename Function>
    bool Throws(Function&& function)
    {
        try
        {
            function();
        }
        catch (const std::exception&)
        {
            return true;
        }
        return false;
    }

    PKPackage BuildFixture()
    {
        const std::vector<uint8_t> pixels(16 * 16 * 4, 255);
        const RZTexture texture = RZTexture::FromRGBA8("test texture", pixels.data(), 16, 16);
        PKPackage fixture;
        auto root = CMChunk::Container(Root, 1);
        auto library = CMChunk::Container(Gen_TextureLibrary, 1);
        library.AddChild(texture.BuildLibraryResource(0));
        const uint64_t postOffset = root.HeaderSize() + library.SerializedSize();
        library.children[0] = texture.BuildLibraryResource(postOffset);
        root.AddChild(std::move(library));
        root.AddChild(texture.BuildPostLoadData(postOffset));
        fixture.rootChunks.push_back(std::move(root));
        return fixture;
    }

    PKPackage BuildPCFixture(uint16_t version)
    {
        const std::vector<uint8_t> pixels(16 * 16 * 4, 255);
        const auto texture = RZTexture::FromRGBA8("PC texture", pixels.data(), 16, 16);
        const auto dds = texture.ToDds();
        auto data = CMChunk::Leaf(Texture_Data, version, {});
        data.isLittleEndian = true;
        data.Append<uint32_t>(uint32_t(dds.size() + 4));
        for (int i = 0; i < 3; ++i)
            data.Append(0.0f);
        data.Append(1.0f);
        data.Append<uint32_t>(1);
        data.Append<uint32_t>(0);
        data.AppendBytes(dds.data(), dds.size());
        const bool wide = version == 10;
        const uint16_t headerVersion = version == 5 ? 3 : 5;
        auto resource = CMChunk::Container(GenSub_Resource, 1);
        auto header = CMChunkResourceHeader::Build(texture.nameCRC, 4, 31, 0, 0, 0, texture.name, true, wide,
                                                   headerVersion);
        resource.AddChild(header);
        resource.AddChild(CMChunk::Container(Texture, 6));
        auto library = CMChunk::Container(Gen_TextureLibrary, 1);
        library.AddChild(resource);
        auto root = CMChunk::Container(Root, 1);
        const uint64_t offset = root.HeaderSize() + library.SerializedSize();
        header = CMChunkResourceHeader::Build(texture.nameCRC, 4, 31, 0, offset,
                                              version == 5 ? 0 : RZTexture::PostLoadCRC(data.data, true),
                                              texture.name, true, wide, headerVersion);
        library.children[0].children[0] = header;
        root.AddChild(std::move(library));
        auto post = CMChunk::Container(Gen_PostLoadData, 1);
        post.AddChild(header);
        post.AddChild(std::move(data));
        root.AddChild(std::move(post));
        PKPackage fixture;
        fixture.isLittleEndian = true;
        fixture.rootChunks.push_back(std::move(root));
        return fixture;
    }

    const CMChunk* TextureChunk(const PackageSession& session)
    {
        return session.package->rootChunks[0].children[0].children[0].FindChild(Texture);
    }

    void RunRoundTrip(const PKPackage& fixture, const std::filesystem::path& dir, bool compressed)
    {
        const auto source = dir / (compressed ? "source.pkz" : "source.pak");
        auto bytes = fixture.Serialize();
        if (compressed)
        {
            BUCompress::Options options;
            if (fixture.isLittleEndian)
            {
                options.bigEndian32 = false;
                options.wideOffsets = fixture.rootChunks[0].children[1].children[1].version == 10;
                options.lookupFirst = options.wideOffsets;
            }
            bytes = BUCompress::Compress(bytes, options);
        }
        WriteFile(source, bytes);

        PackageSession session;
        Check(session.Load(source.u8string()), "Fixture did not load");
        session.selected = TextureChunk(session);
        const auto original = SelectedTexture(session);
        const auto header = SelectedTextureHeader(session);

        ExtractTexture(*session.package, header, (dir / "export.png").u8string());
        const auto png = BUPng::Decode(ReadFile(dir / "export.png"));
        Check(png.width == 16 && png.height == 16 && png.rgba == original.DecodeLevel0(), "Export pixels differ");

        const auto ddsPath = dir / "export.DDS";
        ExtractTexture(*session.package, header, ddsPath.u8string());
        auto dds = BUDds::Decode(ReadFile(ddsPath));
        Check(BUDds::DecodeRGBA8(dds) == png.rgba, "DDS export pixels differ");
        for (size_t i = 0; i < dds.levels[0].data.size(); i += 4)
        {
            dds.levels[0].data[i] = 255;
            dds.levels[0].data[i + 1] = 0;
            dds.levels[0].data[i + 2] = 0;
        }
        WriteFile(ddsPath, BUDds::Encode(dds));
        ReplaceTexture(session, header, ddsPath.u8string(), 16, 16);
        const auto ddsPixels = SelectedTexture(session).DecodeLevel0();
        Check(ddsPixels[0] == 0 && ddsPixels[2] == 255, "DDS replacement changed RGB order");
        Check(session.Save(source.u8string()), "DDS replacement could not be saved");
        PackageSession ddsReloaded;
        Check(ddsReloaded.Load(source.u8string()), "DDS replacement could not be reloaded");
        ddsReloaded.selected = TextureChunk(ddsReloaded);
        Check(SelectedTexture(ddsReloaded).DecodeLevel0() == ddsPixels, "Saved DDS pixels differ");

        const auto beforeInvalidDds = session.package->Serialize();
        const auto revision = session.revision;
        WriteFile(ddsPath, std::vector<uint8_t>{'D', 'D', 'S', ' '});
        Check(Throws([&] { ReplaceTexture(session, header, ddsPath.u8string(), 16, 16); }) &&
                session.package->Serialize() == beforeInvalidDds && session.revision == revision,
            "Truncated DDS changed the package");

        std::vector<uint8_t> replacement(8 * 8 * 4, 0);
        for (size_t i = 0; i < replacement.size(); i += 4)
        {
            replacement[i] = 255;
            replacement[i + 3] = 255;
        }
        WriteFile(dir / "replacement.png", BUPng::EncodeRGBA8(replacement.data(), 8, 8));

        const auto textureSelection = session.selected;
        session.selected = &session.package->rootChunks[0];
        ReplaceTexture(session, header, (dir / "replacement.png").u8string(), 16, 16);
        Check(session.selected == &session.package->rootChunks[0], "Explicit texture replacement changed the model selection");

        session.selected = textureSelection;
        const auto changed = SelectedTexture(session);
        Check(session.dirty && changed.desc.d3dFormat == original.desc.d3dFormat && changed.gpuData.size() == original.gpuData.size(),
            "Layout/format changed");
        const auto rgba = changed.DecodeLevel0();
        Check(rgba[0] == 255 && rgba[1] == 0 && rgba[3] == 255, "Replacement pixels incorrect");

        const auto updated = SelectedTextureHeader(session);
        const auto post = session.package->FindRootAtOffset(updated.GetDataOffset());
        const auto crc = updated.version <= 3 && header.GetPostLoadDataCRC() == 0
                             ? 0
                             : RZTexture::PostLoadCRC(post->FindChild(Texture_Data)->data,
                                                      session.package->isLittleEndian);
        Check(updated.GetPostLoadDataCRC() == crc &&
                CMChunkResourceHeader(*post->FindChild(GenSub_ResourceHeader)).GetPostLoadDataCRC() == crc,
            "CRCs differ");

        Check(session.Save(source.u8string()) && !session.dirty, "Save failed");
        PackageSession reloaded;
        Check(reloaded.Load(source.u8string()) && reloaded.package->wasCompressed == compressed, "Saved package did not reload");
        reloaded.selected = TextureChunk(reloaded);
        Check(SelectedTexture(reloaded).DecodeLevel0() == rgba, "Saved pixels differ");
        if (compressed)
        {
            const auto& before = session.package->compressHeader;
            const auto& after = reloaded.package->compressHeader;
            Check(before.bigEndian == after.bigEndian && before.wideOffsets == after.wideOffsets &&
                      before.lookupFirst == after.lookupFirst,
                  "Saving changed the container table layout");
        }

        const auto saveAs = dir / "save-as.pak";
        WriteFile(saveAs, std::vector<uint8_t>(bytes.size(), 9));
        Check(session.Save(saveAs.u8string()), "Save As failed");
        Check(reloaded.Load(saveAs.u8string()), "Save As corrupted an existing target");

        const auto before = session.package->Serialize();
        const bool rejected = Throws([&] {
            ReplaceTexture(session, SelectedTextureHeader(session), (dir / "replacement.png").u8string(), 32, 16);
        });
        Check(rejected && before == session.package->Serialize(), "Invalid dimensions modified package");
    }
    void RecoveredContainerSave(const std::filesystem::path& directory)
    {
        auto fixture = BuildPCFixture(5);
        auto post = fixture.rootChunks[0].children.back();
        fixture.rootChunks[0].children.pop_back();
        const size_t offset = fixture.rootChunks[0].SerializedSize();
        fixture.rootChunks.push_back(std::move(post));
        const auto repaired = fixture.Serialize();
        auto broken = repaired;
        for (int i = 0; i < 4; ++i)
            broken[offset + 8 + i] = uint8_t(uint32_t(broken.size() * 2) >> (i * 8));
        const auto path = directory / "recovered.pak";
        WriteFile(path, broken);
        PackageSession session;
        Check(session.Load(path.u8string()) && session.package->warnings.size() == 1,
              "Legacy texture recovery failed");
        Check(session.status.find("recovered") != std::string::npos, "Recovered input was not reported");
        Check(session.Save(path.u8string()) && ReadFile(path) == repaired,
              "In-place save omitted the recovered container length repair");
        Check(session.Load(path.u8string()) && session.package->warnings.empty(),
              "Repaired package still needs recovery");
    }
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2, "Expected fixture directory");
        const auto dir = std::filesystem::u8path(argv[1]);
        std::filesystem::create_directories(dir);
        const PKPackage fixture = BuildFixture();
        for (bool compressed : {false, true})
        {
            RunRoundTrip(fixture, dir, compressed);
            RunRoundTrip(BuildPCFixture(5), dir, compressed);
            RunRoundTrip(BuildPCFixture(10), dir, compressed);
        }
        RecoveredContainerSave(dir);
        std::cout << "Texture export, replacement, CRC and save tests passed\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
