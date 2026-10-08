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
/// A function with no frame of its own (a leaf, or one that only forwards its
/// arguments) has no prologue to find. When code still takes its address - a
/// callback built with `lis`/`addi`, or a pointer in a data table - and that
/// address follows the end of another function, it is a start as well. The Wii
/// Menu hands such a callback (0x8136C8CC, built at 0x8136D128) to its web
/// engine, which called it and stopped the menu.
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

        foreach (var address in AddressesTaken(dol))
            if (FollowsAFunctionEnd(dol, address))
                found.Add(address);

        return found.ToList();
    }

    /// <summary>
    /// Code addresses the program builds or stores: <c>lis rA, hi</c> followed
    /// within a few instructions by <c>addi rB, rA, lo</c> or <c>ori rB, rA, lo</c>,
    /// and aligned words in the data sections.
    /// </summary>
    private static List<uint> AddressesTaken(DolFile dol)
    {
        var taken = new List<uint>();
        foreach (var section in dol.ExecutableSections)
        {
            var data = section.Data.Span;
            var count = data.Length / 4;
            for (var i = 0; i < count; ++i)
            {
                var instruction = Read(data, i * 4);
                var opcode = instruction >> 26;
                if (opcode != 14 && opcode != 24)
                    continue;
                // addi rD, rA, simm (rA = 0 means li); ori rA, rS, uimm
                var source = opcode == 14 ? (instruction >> 16) & 31u : (instruction >> 21) & 31u;
                if (opcode == 14 && source == 0)
                    continue;
                for (var back = 1; back <= 8 && i - back >= 0; ++back)
                {
                    var earlier = Read(data, (i - back) * 4);
                    if (earlier >> 26 != 15 || ((earlier >> 16) & 31u) != 0 || ((earlier >> 21) & 31u) != source)
                        continue;
                    var high = (earlier & 0xFFFFu) << 16;
                    var low = instruction & 0xFFFFu;
                    taken.Add(opcode == 14 ? high + (uint)(short)low : high | low);
                    break;
                }
            }
        }

        foreach (var section in dol.Sections.Where(static s => !s.IsExecutable))
        {
            var data = section.Data.Span;
            for (var offset = 0; offset + 4 <= data.Length; offset += 4)
                taken.Add(Read(data, offset));
        }
        return taken;
    }

    private static bool FollowsAFunctionEnd(DolFile dol, uint address)
    {
        if ((address & 3u) != 0)
            return false;
        foreach (var section in dol.ExecutableSections)
        {
            var start = section.VirtualAddress;
            if (address < start || address >= start + (uint)section.Data.Length)
                continue;
            var offset = (int)(address - start);
            if (offset == 0)
                return true;
            var data = section.Data.Span;
            // A start is never itself padding, and the instruction before it ends a function.
            return Read(data, offset) != 0u && EndsAFunction(Read(data, offset - 4));
        }
        return false;
    }

    private static uint Read(System.ReadOnlySpan<byte> data, int offset) =>
        (uint)((data[offset] << 24) | (data[offset + 1] << 16) |
               (data[offset + 2] << 8) | data[offset + 3]);
}
