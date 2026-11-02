using System.Buffers.Binary;

namespace Translator.Core.Loading;

/// <summary>
/// The Metrowerks setjmp/longjmp pair, found by its code so every title that
/// links it is covered. A guest longjmp resumes a frame on another stack, which
/// a translated call cannot do: their call sites go through the runtime's
/// GuestJmp (src/cpu/guest_jmp.cpp) instead.
/// </summary>
public sealed record GuestJumpBufferFunctions(IReadOnlySet<uint> Setjmp, IReadOnlySet<uint> Longjmp)
{
    // mflr r5; mfcr r6; stw r5,0(r3); stw r6,4(r3); stw r1,8(r3); stw r2,12(r3); stmw r13,20(r3)
    private static readonly uint[] SetjmpPrologue =
        [0x7CA802A6, 0x7CC00026, 0x90A30000, 0x90C30004, 0x90230008, 0x9043000C, 0xBDA30014];

    // lwz r5,0(r3); lwz r6,4(r3); mtlr r5; mtcrf 0xff,r6; lwz r1,8(r3); lwz r2,12(r3); lmw r13,20(r3)
    private static readonly uint[] LongjmpPrologue =
        [0x80A30000, 0x80C30004, 0x7CA803A6, 0x7CCFF120, 0x80230008, 0x8043000C, 0xB9A30014];

    public static readonly GuestJumpBufferFunctions None = new(new HashSet<uint>(), new HashSet<uint>());

    public static GuestJumpBufferFunctions Find(ProgramImage image)
    {
        var setjmp = new HashSet<uint>();
        var longjmp = new HashSet<uint>();
        var memory = image.Memory;
        for (var offset = 0; offset + SetjmpPrologue.Length * 4 <= memory.Length; offset += 4)
        {
            if (Matches(memory, offset, SetjmpPrologue))
                setjmp.Add(checked(image.MemoryBase + (uint)offset));
            else if (Matches(memory, offset, LongjmpPrologue))
                longjmp.Add(checked(image.MemoryBase + (uint)offset));
        }

        return new GuestJumpBufferFunctions(setjmp, longjmp);
    }

    private static bool Matches(byte[] memory, int offset, uint[] words)
    {
        for (var i = 0; i < words.Length; i++)
        {
            if (BinaryPrimitives.ReadUInt32BigEndian(memory.AsSpan(offset + i * 4, 4)) != words[i])
                return false;
        }

        return true;
    }
}
