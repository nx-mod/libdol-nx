using System.Text.Json;
using System.Text.RegularExpressions;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;

namespace Translator.Core;

// todo: the entire reading of cpp/h files using regex is a bit fragile, but it works for now. 
// however this does eventually need to be replaced with another solution.
public sealed record RuntimeNativeRegistration(
    uint Address,
    string Symbol,
    string SourceFile,
    bool IsTranslatedOverride,
    bool ExcludesBaseTranslation);

public sealed record RuntimeNativeAbiEntry(
    uint Address,
    string[] ArgumentRegisters,
    string[] ScalarFloatArgumentRegisters);

public sealed record RuntimeNativeEffectEntry(
    uint Address,
    GuestAbiContract Contract,
    bool IsPrecise);

/// <summary>
/// One process-local view of the runtime's native registrations and their guest
/// ABI contracts. C++ remains the only source of truth: the translator builds
/// this index once, shares it across all consumers, and writes nothing to disk.
/// </summary>
public sealed record RuntimeNativeIndex(
    RuntimeNativeRegistration[] Registrations,
    RuntimeNativeAbiEntry[] VoidStubAbis,
    RuntimeNativeEffectEntry[] Effects)
{
    public RuntimeNativeGuestEffectSet ToGuestEffectSet()
    {
        var contracts = Effects.ToDictionary(static entry => entry.Address, static entry => entry.Contract);
        var precise = Effects.Where(static entry => entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        var conservative = Effects.Where(static entry => !entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        return new RuntimeNativeGuestEffectSet(contracts, precise, conservative);
    }
}

public static class RuntimeNativeIndexBuilder
{
    /// <summary>
    /// Scans the runtime for the guest functions it replaces.
    /// </summary>
    /// <param name="nativeSourceDirectory">The runtime's sources.</param>
    /// <param name="bindingsPath">
    /// Optional: a game's own addresses for those functions, as written by
    /// example-wii-nx's resolve-symbols ({"symbol": "0x8012ABCD"}). The
    /// addresses in the sources belong to the game the runtime was written
    /// against, so translating a different game needs its own. A replacement the
    /// file does not name is dropped: that game's own code is translated instead.
    /// </param>
    public static RuntimeNativeIndex Build(string nativeSourceDirectory, string? bindingsPath = null,
                                           string? gameNativeDirectory = null)
    {
        var sourceRoot = Path.GetFullPath(nativeSourceDirectory);
        if (!Directory.Exists(sourceRoot))
            return new RuntimeNativeIndex([], [], []);

        var engineSources = NativeSourceParsing.ReadDirectory(sourceRoot).ToList();
        // The console's own libraries are compiled into the runtime beside this
        // one - Runtime.cmake finds them the same way - and they register
        // natives too: IOS, the NAND and the Wii Remote are all libwii-nx's. A
        // translator that cannot see them translates the functions they
        // replace, and then both claim the address at run time.
        foreach (var console in ConsoleLibraryRoots(sourceRoot))
            engineSources.AddRange(NativeSourceParsing.ReadDirectory(console));
        // A game's own replacements live with the game, not in the engine, and
        // are just as much a reason not to translate a function.
        var gameSources = !string.IsNullOrWhiteSpace(gameNativeDirectory) && Directory.Exists(gameNativeDirectory)
            ? NativeSourceParsing.ReadDirectory(Path.GetFullPath(gameNativeDirectory)).ToList()
            : [];
        var sources = engineSources.Concat(gameSources).ToList();
        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeSources(sources);
        var abis = RuntimeNativeFunctionAbiProvider.AnalyzeVoidStubAbis(sources);
        var bindings = LoadBindings(bindingsPath);
        return new RuntimeNativeIndex(
            // Only the engine's registrations are rebound: their addresses belong
            // to the game the runtime was written against. A game's own native
            // directory is already written for this game, so its addresses are
            // used as they stand - rebinding would drop them, and the function
            // would be both translated and replaced at the same address.
            Rebind(ScanRegistrations(engineSources), bindings)
                .Concat(ScanRegistrations(gameSources))
                .OrderBy(static registration => registration.Address)
                .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
                .ToArray(),
            abis.OrderBy(static item => item.Key)
                .Select(static item => new RuntimeNativeAbiEntry(
                    item.Key,
                    item.Value.ArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray(),
                    item.Value.ScalarFloatArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray()))
                .ToArray(),
            effects.Contracts.OrderBy(static item => item.Key)
                .Select(item => new RuntimeNativeEffectEntry(
                    item.Key, item.Value, effects.PreciseContracts.Contains(item.Key)))
                .ToArray());
    }

    /// <summary>
    /// libwii-nx and libgc-nx, beside the library these sources belong to,
    /// which is where a runtime build looks for them.
    /// </summary>
    private static IEnumerable<string> ConsoleLibraryRoots(string sourceRoot)
    {
        var beside = Path.GetDirectoryName(Path.GetDirectoryName(sourceRoot));
        if (beside is null)
            yield break;
        foreach (var library in new[] { "libwii-nx", "libgc-nx" })
        {
            var path = Path.Combine(beside, library, "src");
            if (Directory.Exists(path))
                yield return path;
        }
    }

    private static IEnumerable<RuntimeNativeRegistration> ScanRegistrations(
        IReadOnlyList<NativeSourceFile> sources)
    {
        var registrations = new List<RuntimeNativeRegistration>();
        foreach (var sourceFile in sources)
        {
            var source = sourceFile.Content;
            foreach (Match match in GeneratedMarkers.NativeFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, !match.Groups["as"].Success);
            foreach (Match match in GeneratedMarkers.TranslatedFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, true, true);
            foreach (Match match in GeneratedMarkers.NativeOverridePattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, true);
            foreach (Match match in GeneratedMarkers.FatalStubPattern().Matches(source))
                Add(match, $"GX_FATAL_STUB_{match.Groups["address"].Value}", false, true);

            void Add(Match match, string symbol, bool translated, bool excludesBase) =>
                registrations.Add(new RuntimeNativeRegistration(
                    ParseAddress(match.Groups["address"].Value), symbol, sourceFile.RelativePath,
                    translated, excludesBase));
        }

        return registrations
            .Distinct()
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
            .ThenBy(static registration => registration.SourceFile, StringComparer.Ordinal);
    }

    private static uint ParseAddress(string value) => GuestTargetParser.ParseHexAddress(value);

    /// <summary>Reads a game's symbol-to-address table, or null when it has none.</summary>
    private static IReadOnlyDictionary<string, uint>? LoadBindings(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;
        if (!File.Exists(path))
            throw new FileNotFoundException($"runtime.native_bindings not found: {path}", path);

        using var stream = File.OpenRead(path);
        using var document = JsonDocument.Parse(stream);
        var bindings = new Dictionary<string, uint>(StringComparer.Ordinal);
        foreach (var entry in document.RootElement.EnumerateObject())
        {
            var text = entry.Value.GetString();
            if (!string.IsNullOrWhiteSpace(text))
                bindings[entry.Name] = GuestTargetParser.ParseHexAddress(text!);
        }

        return bindings;
    }

    /// <summary>
    /// Moves each replacement to where this game keeps it, and drops the ones it
    /// does not have.
    /// </summary>
    private static IEnumerable<RuntimeNativeRegistration> Rebind(
        IEnumerable<RuntimeNativeRegistration> registrations,
        IReadOnlyDictionary<string, uint>? bindings)
    {
        if (bindings is null)
            return registrations;

        var all = registrations.ToList();

        // The same two shapes wiinx-make-bindings accepts, read the same way,
        // so the table the runtime links and the index the translator works
        // from say the same thing. Anything else and one of them replaces a
        // function the other has translated.
        var byReference = bindings
            .Where(entry => entry.Key.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            .ToDictionary(entry => GuestTargetParser.ParseHexAddress(entry.Key),
                          entry => entry.Value);

        IEnumerable<RuntimeNativeRegistration> bound;
        if (byReference.Count != 0)
        {
            bound = all
                .Where(registration => byReference.ContainsKey(registration.Address))
                .Select(registration => registration with { Address = byReference[registration.Address] });
        }
        else
        {
            // One native often replaces several guest functions, and a name
            // standing for more than one registration says nothing about which
            // of them this game's address belongs to. None of them are bound,
            // and the game's own code is translated instead.
            var ambiguous = all
                .GroupBy(registration => registration.Symbol, StringComparer.Ordinal)
                .Where(group => group.Select(registration => registration.Address).Distinct().Count() > 1)
                .Select(group => group.Key)
                .ToHashSet(StringComparer.Ordinal);
            bound = all
                .Where(registration => !ambiguous.Contains(registration.Symbol)
                                       && bindings.ContainsKey(registration.Symbol))
                .Select(registration => registration with { Address = bindings[registration.Symbol] });
        }

        return bound
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal);
    }
}
