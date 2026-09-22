#pragma once

#include "nand_first_run.h"
#include "runtime_config.h"
#include "nand_settings.h"
#include "runtime_log.h"
#include "switch_account_identity.h"
#include "system_bridge.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace RuntimeNandPath {

inline std::optional<std::filesystem::path> ExistingDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    if (!path.empty() && std::filesystem::is_directory(path, ec) && !ec) {
        return path;
    }
    return std::nullopt;
}

[[noreturn]] inline void FailNandRoot(const char* message, const std::filesystem::path& path = {}) {
    if (path.empty()) {
        RT_LOGF(RT_TAG_NAND, "ERROR: %s\n", message);
    } else {
        RT_LOGF(RT_TAG_NAND, "ERROR: %s: %s\n", message,
                RuntimeConfigFile::PathToUtf8(path).c_str());
    }
    RT_LOGF(RT_TAG_NAND, "Set [paths] nand_root in Config.toml.\n");
    std::string details = message ? message : "The configured NAND could not be initialized.";
    if (!path.empty()) {
        details += "\n\nPath: ";
        details += RuntimeConfigFile::PathToUtf8(path);
    }
    details += "\n\nSet [paths] nand_root in Config.toml and try again.";
    // Same fatal idiom as the DVD and OS paths: crash artifacts first so the run
    // folder always has them, then a non-zero exit code, the popup, and the
    // "already reported" latch so the atexit handler does not stack a second
    // generic report on top of this one.
    RuntimeCrash::WriteCrashArtifacts("nand_root", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("NAND initialization failed", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

inline std::filesystem::path ResolveConfiguredPath(const std::string& value) {
    return RuntimeConfigFile::ResolveRelativeToConfig(value);
}

inline std::filesystem::path ManagedNandRootPath() {
#if defined(__SWITCH__)
    // One NAND for every game, like a real Wii (switch_layout.h).
    return std::filesystem::path(SwitchLayout::System("nand"));
#else
    return RuntimeConfigFile::ApplicationDataDirectory() / "NAND";
#endif
}

inline std::optional<std::filesystem::path> BootstrapPayloadPath() {
    if (auto executableDirectory = RuntimeConfigFile::ExecutableDirectory()) {
        const auto adjacent = *executableDirectory / "wii_bootstrap";
        if (ExistingDirectory(adjacent / "shared2" / "wc24")) {
            return adjacent;
        }
    }

#if defined(__SWITCH__)
    // No executable directory or cwd here: the payload is shared by every game,
    // in the wii-nx system folder (switch_layout.h).
    const std::filesystem::path switchBootstrap = SwitchLayout::System("bootstrap");
    if (ExistingDirectory(switchBootstrap / "shared2" / "wc24")) {
        return switchBootstrap;
    }
#endif

#if !defined(__SWITCH__)
    // This makes developer-tree launches work without changing their release layout.
    // No equivalent "walk up from cwd" concept on Switch - there is no cwd
    // (std::filesystem::current_path() throws there; see ExecutableDirectory
    // in runtime_config.h) - so this fallback is simply unavailable there.
    for (auto base = std::filesystem::current_path(); !base.empty();) {
        const auto candidate = base / "runtime" / "assets" / "wii";
        if (ExistingDirectory(candidate / "shared2" / "wc24")) {
            return candidate;
        }
        const auto parent = base.parent_path();
        if (parent == base) {
            break;
        }
        base = parent;
    }
#endif
    return std::nullopt;
}

inline bool CopyBootstrapFile(const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& destinationRoot,
                              const std::filesystem::path& relativePath,
                              std::error_code& ec) {
    const auto source = sourceRoot / relativePath;
    const auto destination = destinationRoot / relativePath;
    if (std::filesystem::exists(destination, ec) && !ec) {
        // Keep whatever the player has - except an empty file where the payload
        // has content. WC24 rejects a zero-length download list or friend list
        // outright, and MKW turns that into a save error; since seeding only
        // ever ran for missing files, such a file stayed broken forever.
        const auto existingSize = std::filesystem::file_size(destination, ec);
        if (ec) {
            return false;
        }
        if (existingSize != 0) {
            return true;
        }
        std::error_code sourceEc;
        const auto sourceSize = std::filesystem::file_size(source, sourceEc);
        if (sourceEc || sourceSize == 0) {
            return true;
        }
        // Fall through and re-copy.
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        return false;
    }

    // std::filesystem::copy_file relies on a fast-copy syscall (sendfile/copy_file_range)
    // that devkitA64's newlib doesn't implement, failing with "Function not implemented"
    // instead of falling back - so copy manually via plain stream I/O instead.
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    out << in.rdbuf();
    out.flush();
    if (!out) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    ec.clear();
    return true;
}

// Create these WC24 files only for a new profile; never overwrite user data.
constexpr std::string_view kBootstrapFiles[] = {
    "shared2/wc24/misc.bin",
    "shared2/wc24/nwc24dl.bin",
    "shared2/wc24/nwc24fl.bin",
    "shared2/wc24/nwc24fls.bin",
    "shared2/wc24/nwc24msg.cbk",
    "shared2/wc24/nwc24msg.cfg",
    "shared2/wc24/mbox/Readme.txt",
    "shared2/wc24/mbox/wc24recv.ctl",
    "shared2/wc24/mbox/wc24recv.mbx",
    "shared2/wc24/mbox/wc24send.ctl",
    "shared2/wc24/mbox/wc24send.mbx",
};

// Add first-run WC24 files only when the NAND has none yet.
inline bool SeedMissingBootstrapFiles(const std::filesystem::path& root, std::string* outError = nullptr) {
    const auto payload = BootstrapPayloadPath();
    if (!payload) {
        if (outError) {
            *outError = "no bootstrap payload found (checked executable-adjacent wii_bootstrap"
#if defined(__SWITCH__)
                        " and " + RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ApplicationDataDirectory() / "wii_bootstrap")
#endif
                        + ")";
        }
        return false;
    }
    std::error_code ec;
    for (const std::string_view file : kBootstrapFiles) {
        const std::filesystem::path relativePath{std::string(file)};
        ec.clear();
        if (!CopyBootstrapFile(*payload, root, relativePath, ec)) {
            const std::string details = "could not create " +
                                         RuntimeConfigFile::PathToUtf8(root / relativePath) +
                                         " (payload=" + RuntimeConfigFile::PathToUtf8(*payload) +
                                         ", ec=" + ec.message() + ")";
            RT_LOG(RT_TAG_NAND) << details << std::endl;
            if (outError) {
                *outError = details;
            }
            return false;
        }
    }
    return true;
}

inline std::filesystem::path CreateManagedNandRoot() {
    const std::filesystem::path root = ManagedNandRootPath();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) {
        FailNandRoot("Unable to create managed NAND root", root);
    }

    std::string seedError;
    if (!SeedMissingBootstrapFiles(root, &seedError)) {
        FailNandRoot(("Unable to initialize managed NAND: " + seedError).c_str(), root);
    }

    // The Wii Menu writes these per-console files; a managed NAND has no Wii
    // Menu, so generate them the way Dolphin does. Without the Mii database
    // RFL's read fails, and without SYSCONF every SC getter falls back to a
    // built-in default.
    seedError.clear();
    if (!RuntimeNandFirstRun::SeedGeneratedFiles(root, &seedError)) {
        FailNandRoot(("Unable to initialize managed NAND: " + seedError).c_str(), root);
    }

    const auto marker = root / ".mkw_recompiled_managed_nand";
    ec.clear();
    if (!std::filesystem::exists(marker, ec)) {
        std::ofstream markerFile(marker, std::ios::trunc);
        if (!markerFile) {
            FailNandRoot("Managed NAND root is not writable", root);
        }
        markerFile << "version=1\n";
        markerFile.close();
        if (!markerFile) {
            FailNandRoot("Unable to finish managed NAND initialization", root);
        }
    }

    RT_LOG(RT_TAG_NAND) << "using managed NAND root: " << RuntimeConfigFile::PathToUtf8(root)
                        << std::endl;
    return root;
}

inline std::filesystem::path ResolveNandRootPath() {
    const std::string configPath = RuntimeConfigFile::NandRoot();
    if (!configPath.empty()) {
        const auto path = ResolveConfiguredPath(configPath);
        if (auto existing = ExistingDirectory(path)) {
            // Seed only a new configured NAND so existing frontend data stays unchanged.
            if (!SeedMissingBootstrapFiles(*existing)) {
                RT_LOG(RT_TAG_NAND) << "first-run WC24 seeding failed for the configured NAND root" << std::endl;
            }
            return *existing;
        }
        FailNandRoot("Configured NAND root is not an existing directory", path);
    }
    return CreateManagedNandRoot();
}

inline std::filesystem::path DiscoverNandRootPath() {
    static const auto root = [] {
        const auto resolved = ResolveNandRootPath();
        std::string error;
        std::optional<std::string> presetSerial;
#if defined(__SWITCH__)
        presetSerial = RuntimeSwitchAccount::DerivedNandSerial();
#endif
        if (!RuntimeNandSettings::Ensure(resolved, error, std::time(nullptr), presetSerial)) {
            FailNandRoot(error.c_str(), RuntimeNandSettings::FilePath(resolved));
        }
        return resolved;
    }();
    return root;
}

} // namespace RuntimeNandPath
