using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Translator.Core;

internal sealed record NativeSourceFile(string FullPath, string RelativePath, string Content);

internal static class NativeSourceParsing
{
    public static IReadOnlyList<NativeSourceFile> ReadDirectory(string directory)
    {
        var root = Path.GetFullPath(directory);
        if (!Directory.Exists(root))
            return [];

        return Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
            .Where(static path => path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                                  path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                                  path.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase))
            .Order(StringComparer.OrdinalIgnoreCase)
            .Select(path => new NativeSourceFile(
                path,
                Path.GetRelativePath(root, path).Replace('\\', '/'),
                ApplyBuildDefines(StripCommentsAndLiterals(File.ReadAllText(path)))))
            .ToArray();
    }

    /// <summary>
    /// The build options a runtime is compiled with that change which natives it
    /// registers (recomp.yml <c>runtime.defines</c>). With WIINX_NATIVE_GX the
    /// runtime draws GX itself and leaves the SDK's GX setters to the game's own
    /// code, so the natives it does not compile must not keep those functions
    /// out of the translation either.
    /// </summary>
    public static IReadOnlySet<string> Defines { get; set; } = new HashSet<string>(StringComparer.Ordinal);

    // The only symbols whose conditionals are evaluated: every other #if keeps
    // both branches, as the scan always has, because nothing says which a build
    // takes.
    private static readonly string[] ManagedSymbols = ["WIINX_NATIVE_GX"];

    private static readonly Regex ConditionalPattern = new(
        @"^\s*#\s*(?:(?<kw>ifdef|ifndef)\s+(?<sym>\w+)|if\s+(?<not>!)?\s*defined\s*\(?\s*(?<sym2>\w+)\s*\)?\s*$)",
        RegexOptions.CultureInvariant);

    /// <summary>
    /// Drops the lines a build with <see cref="Defines"/> does not compile, for
    /// the managed symbols only: <c>#if defined(X)</c>, <c>#if !defined(X)</c>,
    /// <c>#ifdef X</c>, <c>#ifndef X</c>, their <c>#else</c> and <c>#endif</c>.
    /// </summary>
    public static string ApplyBuildDefines(string content)
    {
        if (content.IndexOf('#') < 0)
            return content;
        var lines = content.Split('\n');
        var output = new StringBuilder(content.Length);
        // Each open conditional: whether it is one this evaluates, and whether its
        // current branch is compiled.
        var frames = new List<(bool Managed, bool Active)>();
        foreach (var line in lines)
        {
            var trimmed = line.TrimStart();
            var keep = frames.TrueForAll(static f => !f.Managed || f.Active);
            if (trimmed.StartsWith('#'))
            {
                var directive = trimmed[1..].TrimStart();
                var match = ConditionalPattern.Match(line);
                if (match.Success)
                {
                    var symbol = match.Groups["sym"].Success ? match.Groups["sym"].Value : match.Groups["sym2"].Value;
                    if (Array.IndexOf(ManagedSymbols, symbol) >= 0)
                    {
                        var negated = match.Groups["kw"].Value == "ifndef" || match.Groups["not"].Success;
                        frames.Add((true, Defines.Contains(symbol) != negated));
                        output.Append('\n');
                        continue;
                    }
                    frames.Add((false, true));
                }
                else if (directive.StartsWith("if", StringComparison.Ordinal))
                {
                    frames.Add((false, true));
                }
                else if (directive.StartsWith("else", StringComparison.Ordinal) && frames.Count > 0 && frames[^1].Managed)
                {
                    frames[^1] = (true, !frames[^1].Active);
                    output.Append('\n');
                    continue;
                }
                else if (directive.StartsWith("endif", StringComparison.Ordinal) && frames.Count > 0)
                {
                    var managed = frames[^1].Managed;
                    frames.RemoveAt(frames.Count - 1);
                    if (managed)
                    {
                        output.Append('\n');
                        continue;
                    }
                }
            }
            // Dropped lines keep their newline, so positions in a file stay where
            // they were for anything that reports one.
            output.Append(keep ? line : string.Empty).Append('\n');
        }
        if (output.Length > 0)
            output.Length--;
        return output.ToString();
    }

    public static IEnumerable<string> SplitArguments(string arguments)
    {
        var start = 0;
        var depth = 0;
        for (var index = 0; index < arguments.Length; index++)
        {
            var character = arguments[index];
            if (character is '(' or '[' or '<') depth++;
            else if (character is ')' or ']' or '>') depth = Math.Max(0, depth - 1);
            else if (character == ',' && depth == 0)
            {
                yield return arguments[start..index];
                start = index + 1;
            }
        }
        yield return arguments[start..];
    }

    public static bool IsFloatingPointValueArgument(string argument) =>
        !argument.Contains('*', StringComparison.Ordinal) &&
        !argument.Contains('&', StringComparison.Ordinal) &&
        Regex.IsMatch(argument, @"\b(float|double)\b", RegexOptions.CultureInvariant);

    public static string StripCommentsAndLiterals(string content)
    {
        var output = new StringBuilder(content.Length);
        var inLineComment = false;
        var inBlockComment = false;
        var inLiteral = false;
        var quote = '\0';
        for (var index = 0; index < content.Length; index++)
        {
            var character = content[index];
            var next = index + 1 < content.Length ? content[index + 1] : '\0';
            if (inLineComment)
            {
                if (character == '\n') { inLineComment = false; output.Append(character); }
                continue;
            }
            if (inBlockComment)
            {
                if (character == '*' && next == '/') { inBlockComment = false; index++; }
                continue;
            }
            if (inLiteral)
            {
                output.Append(' ');
                if (character == '\\' && index + 1 < content.Length) { output.Append(' '); index++; }
                else if (character == quote) inLiteral = false;
                continue;
            }
            if (character == '/' && next == '/') { inLineComment = true; index++; continue; }
            if (character == '/' && next == '*') { inBlockComment = true; index++; continue; }
            if (character is '\'' or '"') { inLiteral = true; quote = character; output.Append(' '); continue; }
            output.Append(character);
        }
        return output.ToString();
    }
}
