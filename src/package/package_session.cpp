#include "package/package_session.h"
#include "platform/platform.h"
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
    std::string DescribePackage(const std::string& path, const PKPackage& package)
    {
        std::string description = path + (package.wasCompressed ? " (compressed, " : " (raw, ") +
                                  (package.isLittleEndian ? "little-endian)" : "big-endian)");
        if (!package.warnings.empty())
            description += " - recovered " + std::to_string(package.warnings.size()) +
                           " malformed texture container(s); save to repair their lengths";
        return description;
    }

    std::optional<size_t> PatchInPlace(const std::filesystem::path& target, const std::filesystem::path& source,
        const std::vector<uint8_t>& before, const std::vector<uint8_t>& after)
    {
        std::error_code error;
        if (!std::filesystem::equivalent(target, source, error) || before.size() != after.size() ||
            std::filesystem::file_size(target, error) != after.size())
            return std::nullopt;

        std::fstream file(target, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open())
            return std::nullopt;

        size_t changed = 0;
        for (size_t i = 0; i < after.size();)
        {
            if (after[i] == before[i])
            {
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < after.size() && after[i] != before[i])
                ++i;
            file.seekp(static_cast<std::streamoff>(start));
            file.write(reinterpret_cast<const char*>(after.data() + start), static_cast<std::streamsize>(i - start));
            changed += i - start;
        }
        file.close();
        if (!file)
            throw std::runtime_error("Write failed.");
        return changed;
    }
}

bool PackageSession::Load(const std::string& packagePath)
{
    try
    {
        auto candidate = std::make_unique<PKPackage>();
        const auto bytes = ReadFile(std::filesystem::u8path(packagePath));
        candidate->ReadFromMemoryAutoEndian(bytes);
        if (candidate->rootChunks.empty())
            throw std::runtime_error("Package contains no chunks: " + packagePath);

        auto snapshot = candidate->wasCompressed ? candidate->Serialize() : bytes;
        status = DescribePackage(packagePath, *candidate);
        package = std::move(candidate);
        savedBytes = std::move(snapshot);
        path = packagePath;
        selected = &package->rootChunks.front();
        dirty = false;
        ++revision;
        error.clear();
        return true;
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }
}

bool PackageSession::BeginLoad(const std::string& packagePath)
{
    if (IsLoading())
        return false;
    try
    {
        pending = std::async(std::launch::async, [packagePath] {
            auto result = std::make_unique<PackageSession>();
            result->Load(packagePath);
            return result;
        });
        error.clear();
        return true;
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }
}

void PackageSession::PollLoad()
{
    if (!IsLoading() || pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try
    {
        auto result = pending.get();
        if (result->package)
        {
            package = std::move(result->package);
            savedBytes = std::move(result->savedBytes);
            path = std::move(result->path);
            status = std::move(result->status);
            selected = &package->rootChunks.front();
            dirty = false;
            ++revision;
        }
        error = std::move(result->error);
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
    }
}

bool PackageSession::Save(const std::string& destination)
{
    if (!package || IsLoading())
        return false;
    try
    {
        package->ComputeLengths();
        auto bytes = package->Serialize();
        const auto output = std::filesystem::u8path(destination);
        std::string result;
        if (package->wasCompressed)
        {
            WriteFile(output, BUCompress::Compress(bytes, package->ContainerOptions()));
            result = "Saved (recompressed full file)";
        }
        else if (const auto changed = PatchInPlace(output, std::filesystem::u8path(path), savedBytes, bytes))
            result = "Saved (patched " + std::to_string(*changed) + " differing byte(s))";
        else
        {
            WriteFile(output, bytes);
            result = "Saved (full rewrite)";
        }
        savedBytes = std::move(bytes);
        path = destination;
        dirty = false;
        status = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }
}

std::optional<CMChunkResourceHeader> PackageSession::SelectedResource() const
{
    if (!package || !selected)
        return std::nullopt;
    return CMChunkResourceHeader::FindOwner(*package, *selected);
}

void PackageSession::MarkEdited(std::string message)
{
    package->ComputeLengths();
    dirty = true;
    ++revision;
    status = std::move(message);
}
