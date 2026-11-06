using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;

// Extractor for the IGS ROFS v2 images used by the E2100 kernels.
// Format facts implemented here are documented in IGS_ROFS_REVERSE_ENGINEERING.md;
// keys and constants per game are collected in IGS_KEYS.md.

if (args.Length < 1 || args[0] is "-h" or "--help")
{
    Console.Error.WriteLine("Usage: IgsRofsExtractor <p2.img> [--list] [--extract <directory>] [--profile <name>]");
    Console.Error.WriteLine($"Profiles: {string.Join(", ", RofsProfile.All.Select(p => p.Name))} (default: detect)");
    return;
}

var imagePath = args[0];
var listOnly = args.Skip(1).Contains("--list");
var extractAt = Array.IndexOf(args, "--extract");
var outputDirectory = extractAt >= 0 && extractAt + 1 < args.Length ? args[extractAt + 1] : null;
var profileAt = Array.IndexOf(args, "--profile");
var profileName = profileAt >= 0 && profileAt + 1 < args.Length ? args[profileAt + 1] : null;
if (!listOnly && outputDirectory is null)
    listOnly = true;

var image = await File.ReadAllBytesAsync(imagePath);
var rofs = RofsImage.Open(image, profileName);
Console.WriteLine($"Profile: {rofs.Profile.Name}; ROFS magic: 0x{rofs.Magic:X8}; records: {rofs.Records.Count}; metadata ends: 0x{rofs.MetadataEnd:X}");

if (listOnly)
    foreach (var record in rofs.Records)
        Console.WriteLine($"0x{record.Offset:X6} {record.Mode:X4} {record.Size,9} table=0x{record.TableOffset:X8} {record.Name}");

if (outputDirectory is not null)
{
    await rofs.Extract(outputDirectory);
}

/// <summary>
/// Per-kernel variant of the format.  Both share the AES-128 key transform,
/// the record stream and the per-page zlib layout; they differ in constants,
/// field positions, name encryption and the page-table encoding.
/// </summary>
sealed record RofsProfile(
    string Name,
    string? Key,
    int MagicBlock, int MagicWord,
    int FirstRecord, int HeaderSize, int ModeOffset,
    (int Block, int Word, uint Xor) NameLength,
    (int Block, int Word, uint Xor) Size,
    (int Block, int Word, uint Xor) Table,
    bool EncryptedNames,
    bool EncryptedPageTable,
    bool BlankedZlibHeader,
    int PageTableWord = 0,
    uint NameMask = 0xFF, int NameUnit = 1,
    // compressed pages longer than this have their first EncryptedHead
    // bytes AES-CBC encrypted (zero IV, the same key); 0: none
    int EncryptedHead = 0)
{
    public static readonly RofsProfile[] All =
    [
        // Harpoon Lagoon (Fish4P-OS).  XORs are the init constants - 0x3CE8.
        new("fish4p", "Hunter4PFishV1.0",
            MagicBlock: 0xC8, MagicWord: 1,
            FirstRecord: 0x140, HeaderSize: 0x68, ModeOffset: 0x64,
            NameLength: (0x20, 1, 0x3A9A44A4),
            Size: (0x40, 1, 0x3CED596B),
            Table: (0x30, 2, 0x9F3EE741),
            EncryptedNames: false, EncryptedPageTable: false, BlankedZlibHeader: false),
        // Ocean Party (OceanParty-OS).  XORs are the init constants -/+ 0x805E.
        // Names are AES-CBC (zero IV, same key); page-table entries are one
        // encrypted 16-byte block each; each page's zlib header is zeroed.
        new("oceanparty", "PartyOcean6PV1.0",
            MagicBlock: 0x58, MagicWord: 0,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x74,
            NameLength: (0x50, 3, 0x02C4290D),
            Size: (0x30, 1, 0x02C529C9),
            Table: (0x00, 3, 0x03CB8124),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true),
        // Hero of Robots (HeroOfRobots-OS).  Harpoon's layout with other field
        // positions; XORs are the init constants +/- 0x265B.  Names are plain
        // with junk padding; the root record alone uses table XOR 0xF487E415,
        // which the root-range rule below makes unnecessary.
        new("heroofrobots", "RobotsHeroOfV1.0",
            MagicBlock: 0xF0, MagicWord: 3,
            FirstRecord: 0x140, HeaderSize: 0x68, ModeOffset: 0x64,
            NameLength: (0x40, 2, 0x51223E7A),
            Size: (0x30, 0, 0xEF250457),
            Table: (0x00, 2, 0xF487975F),
            EncryptedNames: false, EncryptedPageTable: false, BlankedZlibHeader: false),
        // Scratch (Scratch-OS).  Ocean Party's design; XORs are the init
        // constants +/- 0xA631.  The magic block starts at 0x74 (unaligned),
        // modes are at +0x72, page ends are word 1 of each table block.  The
        // per-file CRC-32C (dec(R+0x10)) is not checked here.
        new("scratch", "SwinV1.00Scratch",
            MagicBlock: 0x74, MagicWord: 2,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x72,
            NameLength: (0x00, 3, 0x2C858D5A),
            Size: (0x30, 1, 0x2C858D5A),
            Table: (0x50, 1, 0xE1ECA651),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 1),
        // Percussion Master 3 (PM3-OS, E2200).  Ocean Party's design; XORs
        // are the init constants - 0xED2E; page ends are word 1.
        new("pmaster3", "E2200-PMaster-V3",
            MagicBlock: 0xD8, MagicWord: 1,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x74,
            NameLength: (0x20, 1, 0x498C027F),
            Size: (0x50, 3, 0xCD83B702),
            Table: (0x40, 0, 0xEC285CDA),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 1),
        // Fish Hunter 4 M400CN (Fish4P-OS build #38).  Harpoon's key and
        // constants, an earlier record layout: header 0x58, mode at +0x54.
        new("fish4p_m400", "Hunter4PFishV1.0",
            MagicBlock: 0xB8, MagicWord: 1,
            FirstRecord: 0x130, HeaderSize: 0x58, ModeOffset: 0x54,
            NameLength: (0x10, 1, 0x3A9A44A4),
            Size: (0x30, 1, 0x3CED596B),
            Table: (0x20, 2, 0x9F3EE741),
            EncryptedNames: false, EncryptedPageTable: false, BlankedZlibHeader: false),
        // Fish Hunter 4 / 6 (Fish-OS).  No AES (Key null): a cramfs-like
        // plaintext stream, 24-byte headers, name length (word 0 & 0x3F) * 4,
        // names padded to 4.  XORs are the init constants - 0x2E9B.
        new("fish", null,
            MagicBlock: 0x78, MagicWord: 0,
            FirstRecord: 0xC0, HeaderSize: 0x18, ModeOffset: 0x10,
            NameLength: (0x00, 0, 0),
            Size: (0x0C, 0, 0xBE5BE76E),
            Table: (0x14, 0, 0xED8D65C8),
            EncryptedNames: false, EncryptedPageTable: false, BlankedZlibHeader: false,
            NameMask: 0x3F, NameUnit: 4),
        // Fish Hunter 6 M607CN (Fish6Pv2-OS).  Ocean Party's design; XORs
        // are the init constants +/- 0xC7; page ends are word 3.
        new("fish6pv2", "Fish6PHunterV2.0",
            MagicBlock: 0xD0, MagicWord: 1,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x72,
            NameLength: (0x00, 0, 0x683CD4AC),
            Size: (0x30, 0, 0x67CC9C41),
            Table: (0x50, 3, 0xC2693974),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 3),
        // Fish Hunter SKL (FishSKL-OS).  Ocean Party's design; XORs are the
        // init constants - 0x85D0; page ends are word 1.
        new("fishskl", "SKLHunterFish-V1",
            MagicBlock: 0xD8, MagicWord: 0,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x74,
            NameLength: (0x20, 0, 0x01655DAF),
            Size: (0x50, 0, 0xACA3EBBD),
            Table: (0x40, 1, 0xA4BFC10A),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 1),
        // Fish Hunter SKL2 (FishSKL2-OS).  Ocean Party's design; XORs are the
        // init constants +/- 0xAADE; page ends are word 0; the first 0xA0
        // bytes of a longer page are encrypted.
        new("fishskl2", "OSFSFishSKLv2-RF",
            MagicBlock: 0xD0, MagicWord: 2,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x72,
            NameLength: (0x00, 0, 0x25B70D1E),
            Size: (0x30, 1, 0x2A4581CF),
            Table: (0x50, 3, 0x8FE433E6),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 0, EncryptedHead: 0xA0),
        // Demon Hunter (DemonHunter-OS).  Ocean Party's superblock and record
        // size, Harpoon's plain names and page table; XORs are the init
        // constants - 0xA37E.
        new("demonhunter", "Hunter_DemonV1.0",
            MagicBlock: 0xD8, MagicWord: 3,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x74,
            NameLength: (0x20, 1, 0xD02D3979),
            Size: (0x50, 1, 0xBEEF2E50),
            Table: (0x40, 2, 0x7587D5D4),
            EncryptedNames: false, EncryptedPageTable: false, BlankedZlibHeader: false),
        // Crazy Shoot (CrazyShoot-OS).  SKL2's design with its own positions;
        // page ends are word 2.
        new("crazyshoot", "LsT9eNJaHQb2zmMw",
            MagicBlock: 0x100, MagicWord: 3,
            FirstRecord: 0x150, HeaderSize: 0x78, ModeOffset: 0x74,
            NameLength: (0x50, 1, 0x1F011239),
            Size: (0x30, 3, 0xA1075718),
            Table: (0x00, 1, 0xBAA513A0),
            EncryptedNames: true, EncryptedPageTable: true, BlankedZlibHeader: true,
            PageTableWord: 2, EncryptedHead: 0xA0),
    ];
}

sealed class RofsImage
{
    private const uint RofsMagic = 0xD4AA2684;
    private readonly byte[] _image;
    private readonly Aes _aes;
    private readonly ICryptoTransform _decryptor;
    public RofsProfile Profile { get; }
    public uint Magic { get; }
    public List<Record> Records { get; } = [];
    public int MetadataEnd { get; private set; }

    public static RofsImage Open(byte[] image, string? profileName)
    {
        var candidates = profileName is null
            ? RofsProfile.All
            : RofsProfile.All.Where(p => p.Name == profileName).ToArray();
        if (candidates.Length == 0)
            throw new ArgumentException($"Unknown profile '{profileName}'.");
        foreach (var profile in candidates)
        {
            var rofs = new RofsImage(image, profile);
            if (rofs.Magic == RofsMagic)
            {
                rofs.ScanRecords();
                return rofs;
            }
        }
        throw new InvalidDataException("Not an IGS ROFS v2 image for any known profile (no key decodes the superblock magic).");
    }

    private RofsImage(byte[] image, RofsProfile profile)
    {
        _image = image;
        Profile = profile;
        // The driver transforms the key literal before its normal AES-128 expansion.
        var source = System.Text.Encoding.ASCII.GetBytes(profile.Key ?? "plaintext-image!");
        var key = new byte[16];
        BinaryPrimitives.WriteUInt32BigEndian(key.AsSpan(0, 4), BinaryPrimitives.ReadUInt32BigEndian(source.AsSpan(0, 4)) >> 2);
        BinaryPrimitives.WriteUInt32BigEndian(key.AsSpan(4, 4), BinaryPrimitives.ReadUInt32BigEndian(source.AsSpan(4, 4)) >> 1);
        BinaryPrimitives.WriteUInt32BigEndian(key.AsSpan(8, 4), BinaryPrimitives.ReadUInt32BigEndian(source.AsSpan(8, 4)));
        BinaryPrimitives.WriteUInt32BigEndian(key.AsSpan(12, 4), BinaryPrimitives.ReadUInt32BigEndian(source.AsSpan(12, 4)) << 1);
        _aes = Aes.Create();
        _aes.Key = key;
        _aes.Mode = CipherMode.ECB;
        _aes.Padding = PaddingMode.None;
        _decryptor = _aes.CreateDecryptor();

        Magic = Word(DecryptBlock(profile.MagicBlock), profile.MagicWord * 4);
    }

    private void ScanRecords()
    {
        var offset = Profile.FirstRecord;
        while (offset + Profile.HeaderSize <= _image.Length)
        {
            Record record;
            try { record = DecodeRecord(offset); }
            catch { break; }
            if (record.Name.Length == 0 || record.Name.Any(char.IsControl)) break;
            Records.Add(record);
            offset += Profile.HeaderSize + AlignName(record.NameLength);
        }
        MetadataEnd = offset;
    }

    private uint Field((int Block, int Word, uint Xor) field, int offset) =>
        Word(DecryptBlock(offset + field.Block), field.Word * 4) ^ field.Xor;

    private Record DecodeRecord(int offset)
    {
        var nameLength = (int)(Field(Profile.NameLength, offset) & Profile.NameMask) * Profile.NameUnit;
        var nameStart = offset + Profile.HeaderSize;
        if (nameLength is <= 0 or > 255 || nameStart + AlignName(nameLength) > _image.Length)
            throw new InvalidDataException("Invalid record name length.");
        var nameBytes = _image.AsSpan(nameStart, AlignName(nameLength)).ToArray();
        if (Profile.EncryptedNames)
            nameBytes = _aes.DecryptCbc(nameBytes, new byte[16], PaddingMode.None);
        var name = System.Text.Encoding.ASCII.GetString(nameBytes, 0, nameLength).TrimEnd('\0');
        var size = Field(Profile.Size, offset) & 0x0FFF_FFFF;
        var table = (Field(Profile.Table, offset) & 0x03FF_FFFF) * 4U;
        var mode = BinaryPrimitives.ReadUInt16LittleEndian(_image.AsSpan(offset + Profile.ModeOffset, 2));
        return new Record(offset, name, nameLength, mode, size, table);
    }

    public async Task Extract(string outputDirectory)
    {
        Directory.CreateDirectory(outputDirectory);
        var used = new HashSet<int>();
        var nodes = new List<string>();
        // The mount record is held in the superblock.  Its children are the
        // records from FirstRecord up to the first child range of any directory.
        var rootEnd = Records.Where(r => r.IsDirectory && r.Size != 0).Min(r => r.TableOffset);
        await ExtractDirectory(outputDirectory, (uint)Profile.FirstRecord, rootEnd - (uint)Profile.FirstRecord, used, nodes);
        var missing = Records.Where(r => !used.Contains(r.Offset)).ToList();
        if (missing.Count != 0)
            throw new InvalidDataException($"Tree reconstruction missed {missing.Count} metadata records.");
        await File.WriteAllLinesAsync(Path.Combine(outputDirectory, "IGS_ROFS_SPECIAL_NODES.txt"), nodes);
        Console.WriteLine($"Extracted {used.Count} metadata entries into {Path.GetFullPath(outputDirectory)}.");
    }

    private async Task ExtractDirectory(string hostDirectory, uint start, uint length, HashSet<int> used, List<string> nodes)
    {
        if (start < Profile.FirstRecord || start + length > MetadataEnd || (start & 3) != 0)
            throw new InvalidDataException($"Invalid directory range 0x{start:X}+0x{length:X}.");
        for (var offset = (int)start; offset < start + length;)
        {
            if (!RecordsByOffset.TryGetValue(offset, out var record))
                throw new InvalidDataException($"No metadata record at 0x{offset:X}.");
            if (!used.Add(offset))
                throw new InvalidDataException($"Directory graph reuses record 0x{offset:X} ({record.Name}).");
            var path = Path.Combine(hostDirectory, Sanitize(record.Name));
            if (record.IsDirectory)
            {
                Directory.CreateDirectory(path);
                if (record.Size != 0)
                    await ExtractDirectory(path, record.TableOffset, record.Size, used, nodes);
            }
            else if (record.IsData)
            {
                var bytes = ReadContent(record);
                if (record.IsSymlink)
                {
                    var target = System.Text.Encoding.ASCII.GetString(bytes);
                    try { File.CreateSymbolicLink(path, target); }
                    catch (UnauthorizedAccessException) { nodes.Add($"SYMLINK {path} -> {target} (not created: insufficient privilege)"); }
                }
                else
                {
                    await File.WriteAllBytesAsync(path, bytes);
                }
            }
            else
            {
                nodes.Add($"mode={record.Mode:X4} metadata=0x{record.Offset:X} path={path}");
            }
            offset += Profile.HeaderSize + AlignName(record.NameLength);
        }
    }

    private Dictionary<int, Record> RecordsByOffset => _recordsByOffset ??= Records.ToDictionary(r => r.Offset);
    private Dictionary<int, Record>? _recordsByOffset;

    /// <summary>Page-table entry: compressed end offset of page <paramref name="page"/>.</summary>
    private uint PageEnd(Record record, int page) => Profile.EncryptedPageTable
        ? Word(DecryptBlock(checked((int)record.TableOffset + page * 16)), Profile.PageTableWord * 4)
        : ReadImageWord(checked((int)record.TableOffset + page * 4));

    private byte[] ReadContent(Record record)
    {
        if (!record.IsData) throw new InvalidOperationException("Not data-bearing.");
        if (record.Size == 0) return [];
        var result = new byte[record.Size];
        var pages = ((int)record.Size + 0xFFF) >> 12;
        var entrySize = Profile.EncryptedPageTable ? 16U : 4U;
        for (var page = 0; page < pages; page++)
        {
            var end = PageEnd(record, page);
            // For page zero the preceding extent is the page table itself.
            var start = page == 0
                ? record.TableOffset + (uint)pages * entrySize
                : PageEnd(record, page - 1);
            if (end <= start || end - start > 0x2000 || end > _image.Length)
                throw new InvalidDataException($"Invalid compressed extent for {record.Name}, page {page}: 0x{start:X}-0x{end:X}");
            var compressed = _image.AsSpan((int)start, (int)(end - start)).ToArray();
            if (Profile.EncryptedHead > 0 && compressed.Length > Profile.EncryptedHead)
                _aes.DecryptCbc(compressed.AsSpan(0, Profile.EncryptedHead), new byte[16],
                    compressed.AsSpan(0, Profile.EncryptedHead), PaddingMode.None);
            if (Profile.BlankedZlibHeader)
            {
                // The kernel restores the zlib header before inflating.
                compressed[0] = 0x78;
                compressed[1] = 0x9C;
            }
            using var input = new MemoryStream(compressed, writable: false);
            using var zlib = new ZLibStream(input, CompressionMode.Decompress);
            var pageData = new byte[0x1000];
            var wanted = Math.Min(0x1000, result.Length - page * 0x1000);
            var actual = 0;
            while (actual < wanted)
            {
                var read = zlib.Read(pageData, actual, wanted - actual);
                if (read == 0) break;
                actual += read;
            }
            if (actual < wanted) throw new InvalidDataException($"Short zlib block for {record.Name}, page {page} ({actual}/{wanted}).");
            Buffer.BlockCopy(pageData, 0, result, page * 0x1000, wanted);
        }
        return result;
    }

    private uint ReadImageWord(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(_image.AsSpan(offset, 4));
    private byte[] DecryptBlock(int offset) => Profile.Key is null
        ? _image.AsSpan(offset, 16).ToArray()
        : _decryptor.TransformFinalBlock(_image, offset, 16);
    private int AlignName(int value) => Profile.Key is null ? (value + 3) & ~3 : Align16(value);
    private static uint Word(byte[] block, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(offset, 4));
    private static int Align16(int value) => (value + 15) & ~15;
    private static string Sanitize(string value)
    {
        var invalid = Path.GetInvalidFileNameChars();
        return new string(value.Select(c => invalid.Contains(c) ? '_' : c).ToArray());
    }
}

sealed record Record(int Offset, string Name, int NameLength, ushort Mode, uint Size, uint TableOffset)
{
    public bool IsRegular => (Mode & 0xF000) == 0x8000;
    public bool IsDirectory => (Mode & 0xF000) == 0x4000;
    public bool IsSymlink => (Mode & 0xF000) == 0xA000;
    public bool IsData => IsRegular || IsSymlink;
}
