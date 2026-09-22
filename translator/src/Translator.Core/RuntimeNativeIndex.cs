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

        var sources = NativeSourceParsing.ReadDirectory(sourceRoot).ToList();
        // A game's own replacements live with the game, not in the engine, and
        // are just as much a reason not to translate a function.
        if (!string.IsNullOrWhiteSpace(gameNativeDirectory) && Directory.Exists(gameNativeDirectory))
            sources.AddRange(NativeSourceParsing.ReadDirectory(Path.GetFullPath(gameNativeDirectory)));
        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeSources(sources);
        var abis = RuntimeNativeFunctionAbiProvider.AnalyzeVoidStubAbis(sources);
        var bindings = LoadBindings(bindingsPath);
        return new RuntimeNativeIndex(
            Rebind(ScanRegistrations(sources), bindings).ToArray(),
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

        return registrations
            .Where(registration => bindings.ContainsKey(registration.Symbol))
            .Select(registration => registration with { Address = bindings[registration.Symbol] })
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal);
    }
}
