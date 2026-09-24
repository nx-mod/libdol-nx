using System.Collections.Generic;
using System.Linq;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

/// <summary>
/// Function starts found by their shape rather than by being called.
///
/// The call-graph walk reaches everything called from the entry point, and a
/// game's symbol map supplies the rest. A game nobody has mapped has no rest:
/// anything reached only through a vtable or a function pointer is never
/// walked, never named, and never translated. Mega Man 10 translated 4,008
/// functions out of 2.9 MB of code that way, leaving 1.6 MB of it in holes.
///
/// A prologue is recognisable on its own. `stwu r1, -N(r1)` and `mflr r0`
/// appear at the top of a function and almost nowhere else, and one that
/// follows a `blr`, a `b` or padding is where the previous function ended.
/// Measured against Mega Man 9, whose 10,050 starts are known from its map,
/// 97% of what this finds is a function that really is one.
///
/// These are seeds, not conclusions: each is translated speculatively, and one
/// that does not decode is dropped with a count rather than failing the run.
/// </summary>
public static class StructuralFunctionStarts
{
    private const uint Blr = 0x4E800020u;
    private const uint MflrR0 = 0x7C0802A6u;
    private const uint StwuR1Mask = 0xFFFF0000u;
    private const uint StwuR1 = 0x94210000u;

    /// <summary>A prologue: the first instruction of a stack frame.</summary>
    private static bool IsPrologue(uint instruction) =>
        instruction == MflrR0 ||
        // stwu r1, -N(r1): a frame is always pushed downwards, so N is negative.
        ((instruction & StwuR1Mask) == StwuR1 && (instruction & 0x8000u) != 0);

    /// <summary>The end of the function before: a return, a tail call, or padding.</summary>
    private static bool EndsAFunction(uint instruction) =>
        instruction == Blr || instruction == 0u || (instruction >> 26) == 18u;

    public static IReadOnlyList<uint> Find(DolFile dol)
    {
        var found = new SortedSet<uint>();
        foreach (var section in dol.ExecutableSections)
        {
            var data = section.Data.Span;
            for (var offset = 4; offset + 4 <= data.Length; offset += 4)
            {
                var instruction = Read(data, offset);
                if (!IsPrologue(instruction) || !EndsAFunction(Read(data, offset - 4)))
                {
                    continue;
                }

                found.Add(section.VirtualAddress + (uint)offset);
            }
        }

        return found.ToList();
    }

    private static uint Read(System.ReadOnlySpan<byte> data, int offset) =>
        (uint)((data[offset] << 24) | (data[offset + 1] << 16) |
               (data[offset + 2] << 8) | data[offset + 3]);
}
