using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SkyOutfitReader;

internal static class Program
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static int Main(string[] args)
    {
        try
        {
            using var process = FindSkyProcess();
            using var memory = new ProcessMemory(process.Id);
            var scanner = new OutfitScanner(process, memory, args.Contains("--diagnostic"));
            var snapshot = scanner.Scan();
            Console.WriteLine(JsonSerializer.Serialize(snapshot, JsonOptions));
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(JsonSerializer.Serialize(new
            {
                error = exception.Message,
                type = exception.GetType().Name,
            }, JsonOptions));
            return 1;
        }
    }

    private static Process FindSkyProcess()
    {
        var processes = Process.GetProcessesByName("Sky");
        if (processes.Length == 0)
        {
            throw new InvalidOperationException("Sky.exe is not running.");
        }

        foreach (var extra in processes.Skip(1))
        {
            extra.Dispose();
        }

        return processes[0];
    }
}

internal sealed class OutfitScanner
{
    private const int AvatarOutfitOffset = 0x58;
    private const int AvatarActiveOffset = 0xB850;
    private const int AvatarFlagsOffset = 0x109EC;
    private const int OutfitDatabaseOffset = 0x10;
    private const int OutfitBaseIdsOffset = 0x54;
    private const int OutfitOverrideIdsOffset = 0x1D48;
    private const int OutfitOverrideFlagsOffset = 0x1D7C;
    private const int DatabaseSentinelOffset = 0x5080;
    private const int DatabaseBucketsOffset = 0x5090;
    private const int DatabaseMaskOffset = 0x50A8;
    private const int DatabaseRecordsOffset = 0x50C0;
    private const int OutfitRecordSize = 0xE50;
    private const int ScanStride = 8;
    private const int ScanCoreSize = 16 * 1024 * 1024;

    private static readonly string[] SlotNames =
    [
        "Horn",
        "Hair",
        "Mask",
        "Neck",
        "Wing",
        "Body",
        "Feet",
        "Prop",
        "Hat",
        "Face",
    ];

    private readonly Process _process;
    private readonly ProcessMemory _memory;
    private readonly bool _diagnostic;
    private long _structuralCandidates;
    private long _databaseCandidates;
    private readonly List<object> _databaseDiagnostics = [];

    public OutfitScanner(Process process, ProcessMemory memory, bool diagnostic)
    {
        _process = process;
        _memory = memory;
        _diagnostic = diagnostic;
    }

    public OutfitSnapshot Scan()
    {
        var module = _process.MainModule
            ?? throw new InvalidOperationException("Unable to inspect Sky.exe's main module.");
        var candidates = new List<OutfitSnapshot>();

        foreach (var region in _memory.EnumerateReadableRegions())
        {
            if (region.Type is not (NativeMethods.MemPrivate or NativeMethods.MemImage)
                || region.RegionSize < AvatarFlagsOffset + 2)
            {
                continue;
            }

            ScanRegion(region, module, candidates);
        }

        var uniqueCandidates = candidates
            .GroupBy(candidate => candidate.AvatarAddress)
            .Select(group => group.First())
            .ToList();

        if (_diagnostic)
        {
            Console.Error.WriteLine(JsonSerializer.Serialize(new
            {
                structuralCandidates = _structuralCandidates,
                databaseCandidates = _databaseCandidates,
                validatedCandidates = uniqueCandidates.Count,
                databases = _databaseDiagnostics,
            }, new JsonSerializerOptions { WriteIndented = true }));
        }

        return uniqueCandidates.Count switch
        {
            1 => uniqueCandidates[0],
            0 => throw new InvalidOperationException(
                "No local avatar outfit was found. Keep the player loaded in a playable scene and try again."),
            _ => throw new InvalidOperationException(
                $"Found {uniqueCandidates.Count} validated local-avatar candidates; refusing to guess."),
        };
    }

    private void ScanRegion(
        MemoryRegion region,
        ProcessModule module,
        List<OutfitSnapshot> candidates)
    {
        var regionStart = region.BaseAddress;
        var regionEnd = checked(region.BaseAddress + region.RegionSize);
        var requiredTail = AvatarFlagsOffset + sizeof(ushort);

        for (nuint coreStart = regionStart; coreStart < regionEnd;)
        {
            var remaining = regionEnd - coreStart;
            var coreLength = (int)Math.Min((nuint)ScanCoreSize, remaining);
            var readLength = (int)Math.Min((nuint)(coreLength + requiredTail), remaining);

            if (_memory.TryReadBytes(coreStart, readLength, out var buffer))
            {
                ScanBuffer(coreStart, coreLength, buffer, module, candidates);
            }

            coreStart += (nuint)coreLength;
        }
    }

    private void ScanBuffer(
        nuint bufferAddress,
        int coreLength,
        byte[] buffer,
        ProcessModule module,
        List<OutfitSnapshot> candidates)
    {
        var firstAligned = (int)((8 - (bufferAddress & 7)) & 7);
        var scanLimit = Math.Min(coreLength, buffer.Length - AvatarFlagsOffset - sizeof(ushort));

        for (var offset = firstAligned; offset < scanLimit; offset += ScanStride)
        {
            if (buffer[offset + AvatarActiveOffset] == 0)
            {
                continue;
            }

            var avatarFlags = BitConverter.ToUInt16(buffer, offset + AvatarFlagsOffset);
            if ((avatarFlags & 0x08) != 0)
            {
                continue;
            }

            var outfitAddress = (nuint)BitConverter.ToUInt64(buffer, offset + AvatarOutfitOffset);
            if (!ProcessMemory.IsPlausibleUserPointer(outfitAddress))
            {
                continue;
            }

            _structuralCandidates++;
            var avatarAddress = bufferAddress + (nuint)offset;
            var snapshot = TryReadOutfit(module, avatarAddress, avatarFlags, outfitAddress);
            if (snapshot is not null)
            {
                candidates.Add(snapshot);
            }
        }
    }

    private OutfitSnapshot? TryReadOutfit(
        ProcessModule module,
        nuint avatarAddress,
        ushort avatarFlags,
        nuint outfitAddress)
    {
        if (!_memory.TryReadBytes(outfitAddress + 8, 16, out var outfitHeader)
            || (nuint)BitConverter.ToUInt64(outfitHeader, 0) != avatarAddress)
        {
            return null;
        }

        var databaseAddress = (nuint)BitConverter.ToUInt64(outfitHeader, OutfitDatabaseOffset - 8);
        if (!ProcessMemory.IsPlausibleUserPointer(databaseAddress)
            || !_memory.TryReadUInt32(databaseAddress + DatabaseMaskOffset, out var mask)
            || mask == 0
            || mask > 0xFFFFF
            || ((mask + 1) & mask) != 0
            || !_memory.TryReadUInt64(databaseAddress + DatabaseSentinelOffset, out var sentinel)
            || !_memory.TryReadUInt64(databaseAddress + DatabaseBucketsOffset, out var buckets)
            || !ProcessMemory.IsPlausibleUserPointer((nuint)sentinel)
            || !ProcessMemory.IsPlausibleUserPointer((nuint)buckets))
        {
            return null;
        }

        var slots = new List<OutfitSlot>(SlotNames.Length);
        var resolvedCount = 0;
        _databaseCandidates++;

        for (var slot = 0; slot < SlotNames.Length; slot++)
        {
            if (!_memory.TryReadUInt32(outfitAddress + OutfitBaseIdsOffset + (nuint)(slot * 4), out var baseId)
                || !_memory.TryReadUInt32(outfitAddress + OutfitOverrideIdsOffset + (nuint)(slot * 4), out var overrideId)
                || !_memory.TryReadUInt32(outfitAddress + OutfitOverrideFlagsOffset + (nuint)(slot * 4), out var overrideFlag))
            {
                return null;
            }

            var effectiveId = overrideFlag != 0 ? overrideId : baseId;
            var resourceName = effectiveId == 0
                ? null
                : TryResolveResourceName(databaseAddress, mask, (nuint)sentinel, (nuint)buckets, slot, effectiveId);

            if (resourceName is not null)
            {
                resolvedCount++;
            }

            slots.Add(new OutfitSlot(
                slot,
                SlotNames[slot],
                baseId,
                overrideId,
                overrideFlag,
                effectiveId,
                resourceName));
        }

        if (_diagnostic && _databaseDiagnostics.Count < 32)
        {
            _databaseDiagnostics.Add(new
            {
                avatar = ToHex(avatarAddress),
                outfit = ToHex(outfitAddress),
                database = ToHex(databaseAddress),
                mask,
                resolvedCount,
                slots = slots.Select(item => new
                {
                    item.Index,
                    item.BaseId,
                    item.OverrideId,
                    item.OverrideFlag,
                    item.EffectiveId,
                    item.ResourceName,
                }),
            });
        }

        if (resolvedCount < 2)
        {
            return null;
        }

        return new OutfitSnapshot(
            _process.Id,
            ToHex((nuint)module.BaseAddress),
            module.ModuleMemorySize,
            ToHex(avatarAddress),
            avatarFlags,
            ToHex(outfitAddress),
            ToHex(databaseAddress),
            slots);
    }

    private string? TryResolveResourceName(
        nuint databaseAddress,
        uint mask,
        nuint sentinel,
        nuint buckets,
        int slot,
        uint id)
    {
        var hash = id;
        hash ^= hash >> 16;
        hash = unchecked(hash * 0x85EBCA6B);
        hash ^= hash >> 13;
        hash = unchecked(hash * 0xC2B2AE35);
        hash ^= hash >> 16;
        var bucket = hash & mask;
        var bucketAddress = buckets + (nuint)(bucket * 16UL);

        if (!_memory.TryReadUInt64(bucketAddress + 8, out var currentValue)
            || !_memory.TryReadUInt64(bucketAddress, out var firstValue))
        {
            return null;
        }

        var current = (nuint)currentValue;
        var first = (nuint)firstValue;
        if (current == sentinel)
        {
            return null;
        }

        for (var iteration = 0; iteration < 4096; iteration++)
        {
            if (!_memory.TryReadUInt32(current + 0x10, out var nodeId))
            {
                return null;
            }

            if (nodeId == id)
            {
                return ResolveRecord(databaseAddress, current, slot, id);
            }

            if (current == first
                || !_memory.TryReadUInt64(current + 8, out var nextValue)
                || !ProcessMemory.IsPlausibleUserPointer((nuint)nextValue))
            {
                return null;
            }

            current = (nuint)nextValue;
        }

        return null;
    }

    private string? ResolveRecord(nuint databaseAddress, nuint node, int slot, uint id)
    {
        if (!_memory.TryReadUInt32(node + 0x14, out var recordIndex) || recordIndex > 0x100000)
        {
            return null;
        }

        var recordAddress = databaseAddress
            + DatabaseRecordsOffset
            + (nuint)(recordIndex * (ulong)OutfitRecordSize);

        if (!_memory.TryReadUInt32(recordAddress, out var recordId)
            || !_memory.TryReadUInt32(recordAddress + 8, out var recordSlot)
            || recordId != id
            || recordSlot != slot)
        {
            return null;
        }

        return _memory.TryReadMsvcString(recordAddress + 0x10, out var value)
            && value.Length is > 0 and <= 512
            && value.All(character => character is >= ' ' and <= '~')
                ? value
                : null;
    }

    private static string ToHex(nuint value) => $"0x{value:X}";
}

internal sealed class ProcessMemory : IDisposable
{
    private readonly nint _handle;

    public ProcessMemory(int processId)
    {
        _handle = NativeMethods.OpenProcess(
            NativeMethods.ProcessQueryInformation | NativeMethods.ProcessVmRead,
            false,
            processId);

        if (_handle == 0)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed.");
        }
    }

    public IEnumerable<MemoryRegion> EnumerateReadableRegions()
    {
        nuint address = 0;
        var structureSize = (nuint)Marshal.SizeOf<NativeMethods.MemoryBasicInformation>();

        while (NativeMethods.VirtualQueryEx(
                   _handle,
                   address,
                   out var information,
                   structureSize) == structureSize)
        {
            var regionSize = information.RegionSize;
            if (information.State == NativeMethods.MemCommit
                && IsReadableProtection(information.Protect)
                && regionSize > 0)
            {
                yield return new MemoryRegion(
                    (nuint)information.BaseAddress,
                    regionSize,
                    information.Type);
            }

            var nextAddress = (nuint)information.BaseAddress + regionSize;
            if (nextAddress <= address)
            {
                yield break;
            }

            address = nextAddress;
        }
    }

    public bool TryReadBytes(nuint address, int length, out byte[] bytes)
    {
        bytes = new byte[length];
        if (!NativeMethods.ReadProcessMemory(
                _handle,
                address,
                bytes,
                (nuint)length,
                out var bytesRead)
            || bytesRead != (nuint)length)
        {
            bytes = [];
            return false;
        }

        return true;
    }

    public bool TryReadUInt32(nuint address, out uint value)
    {
        if (TryReadBytes(address, sizeof(uint), out var bytes))
        {
            value = BitConverter.ToUInt32(bytes);
            return true;
        }

        value = 0;
        return false;
    }

    public bool TryReadUInt64(nuint address, out ulong value)
    {
        if (TryReadBytes(address, sizeof(ulong), out var bytes))
        {
            value = BitConverter.ToUInt64(bytes);
            return true;
        }

        value = 0;
        return false;
    }

    public bool TryReadMsvcString(nuint address, out string value)
    {
        value = string.Empty;
        if (!TryReadUInt64(address + 0x10, out var length)
            || !TryReadUInt64(address + 0x18, out var capacity)
            || length > 4096
            || capacity < length)
        {
            return false;
        }

        nuint dataAddress;
        if (capacity < 16)
        {
            dataAddress = address;
        }
        else if (!TryReadUInt64(address, out var pointer)
                 || !IsPlausibleUserPointer((nuint)pointer))
        {
            return false;
        }
        else
        {
            dataAddress = (nuint)pointer;
        }

        if (!TryReadBytes(dataAddress, (int)length, out var data))
        {
            return false;
        }

        value = Encoding.UTF8.GetString(data);
        return true;
    }

    public static bool IsPlausibleUserPointer(nuint address)
        => address >= 0x10000 && address < 0x0000800000000000 && (address & 7) == 0;

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeMethods.CloseHandle(_handle);
        }
    }

    private static bool IsReadableProtection(uint protection)
    {
        if ((protection & (NativeMethods.PageGuard | NativeMethods.PageNoAccess)) != 0)
        {
            return false;
        }

        return (protection & 0xFF) is
            NativeMethods.PageReadOnly or
            NativeMethods.PageReadWrite or
            NativeMethods.PageWriteCopy or
            NativeMethods.PageExecuteRead or
            NativeMethods.PageExecuteReadWrite or
            NativeMethods.PageExecuteWriteCopy;
    }
}

internal static partial class NativeMethods
{
    public const uint ProcessVmRead = 0x0010;
    public const uint ProcessQueryInformation = 0x0400;
    public const uint MemCommit = 0x1000;
    public const uint MemPrivate = 0x20000;
    public const uint MemImage = 0x1000000;
    public const uint PageNoAccess = 0x01;
    public const uint PageReadOnly = 0x02;
    public const uint PageReadWrite = 0x04;
    public const uint PageWriteCopy = 0x08;
    public const uint PageExecuteRead = 0x20;
    public const uint PageExecuteReadWrite = 0x40;
    public const uint PageExecuteWriteCopy = 0x80;
    public const uint PageGuard = 0x100;

    [StructLayout(LayoutKind.Sequential)]
    public struct MemoryBasicInformation
    {
        public nint BaseAddress;
        public nint AllocationBase;
        public uint AllocationProtect;
        public ushort PartitionId;
        public ushort Alignment1;
        public nuint RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
        public uint Alignment2;
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    public static partial nint OpenProcess(uint desiredAccess, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool ReadProcessMemory(
        nint process,
        nuint baseAddress,
        [Out] byte[] buffer,
        nuint size,
        out nuint numberOfBytesRead);

    [LibraryImport("kernel32.dll")]
    public static partial nuint VirtualQueryEx(
        nint process,
        nuint address,
        out MemoryBasicInformation information,
        nuint length);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool CloseHandle(nint handle);
}

internal sealed record OutfitSnapshot(
    int ProcessId,
    string ModuleBase,
    int ModuleSize,
    string AvatarAddress,
    ushort AvatarFlags,
    string OutfitAddress,
    string DatabaseAddress,
    IReadOnlyList<OutfitSlot> Slots);

internal sealed record OutfitSlot(
    int Index,
    string Type,
    uint BaseId,
    uint OverrideId,
    uint OverrideFlag,
    uint EffectiveId,
    string? ResourceName);

internal readonly record struct MemoryRegion(nuint BaseAddress, nuint RegionSize, uint Type);
