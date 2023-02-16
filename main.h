#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

const char cmd_line_error[] = "-i <path_to_disk_image> -f <file_system_type> -v {run in verbose mode} -h {search for hidden data}\n" \
                        "\nCurrently Supported file system types:\n <fat12>\n <fat16>\n <fat32>\n" \
                        " <raw> (For Full Disk Images that include the MBR. Not for use with images of a single partitions.)\n\n";

void read_error(void) {
    fprintf(stderr, "Unable to read disk image. Please make sure the file has not been moved or deleted.\n");
    exit(EXIT_FAILURE);
}

// Text Headers when printing MBR to console
const char header[7][10] = {
    "ENTRY#",
    "BOOT",
    "START",
    "END",
    "BLOCKS",
    "ID",
    "TYPE"
};

// Global Data / Data Structures
uint32_t bps = 512; // Bytes Per Sector
uint32_t spc = 0; // Sectors Per Cluster
uint32_t reserved_and_fats = 0;
uint32_t root_dir_off; // Offset in Bytes from start of disk image
// uint32_t cluster2_off;
struct fat_boot_sector* fat_bs;
uint8_t *fat1;
uint8_t *fat2;
uint32_t fat_size_in_bytes;

/**
 * @brief Common partition type codes for MBR entries
 */
enum partition_type {
    FAT12 = 0x1,
    FAT16 = 0x4,
    FAT32_CHS = 0x0B,
    FAT32 = 0x0C, //FAT32 with LBA
    EXTENDED = 0x05,
    EXTENDED_LBA = 0x0F,
    NTFS = 0x7,
    LINUX_SWAP = 0x82,
    LINUX_FILE_SYS = 0x83,
    EMPTY_ENTRY = 0x00,
    RAW = 0
};

/**
 * @brief Common offsets within MBR/EBR table
 */
enum offsets {
    // Master Boot Record Offsets
    MBR_SIG_OFF = 0x1FE, // MBR Signature Field Offset
    MBR_PART1_OFF = 0x1BE, // MBR Partition 1 Field Offset
    MBR_PART2_OFF = 0x1CE, // MBR Partition 2 Field Offset
    MBR_PART3_OFF = 0x1DE, // MBR Partition 3 Field Offset
    MBR_PART4_OFF = 0x1EE, // MBR Partition 4 Field Offset

    // MBR Partition Table Entry Relative Offsets
    BOOT_INDICATOR = 0,
    PARTITION_TYPE = 4,
    STARTING_SECTOR = 8,
    PARTITION_SIZE = 12,

    // Extended Boot Record Offsets
    EBR_PART_TABLE_OFF = 0x1BE,
    EBR_ENTRY_OFF = 0x1BE,
    EBR_NEXT_PART_OFF = 0x1CE,
    ERB_SIG_OFF = 0x1FE,

    // FAT Boot Sector Offsets
    OEM_NAME = 3,
    BYTES_PER_SECTOR = 11,
    SECTORS_PER_CLUSTER = 13,
    RESERVED_AREA_SIZE = 14,
    NUMBER_OF_FATS = 16,
    MAX_FILES_IN_ROOT = 17,
    SECTOR_COUNT_16B = 19,
    MEDIA_TYPE = 21,
    FAT_SIZE_IN_SECTORS = 22,
    SECTORS_PER_TRACK = 24,
    HEAD_NUMBER = 26,
    SECTORS_BEFORE_PARTITION = 28,
    SECTOR_COUNT_32B = 32,
    BIOS_DRIVE_NUMBER = 36,
    EXTENDED_BOOT_SIG = 38,
    VOLUME_SERIAL = 39,
    VOLUME_LABEL = 43,
    FS_TYPE_LABEL = 54,
    FS_SIGNATURE = 510,

    // FAT32 Boot Sector Extended Offsets
    FAT32_SIZE_IN_SECTORS = 36,
    FAT_MODE = 40,
    FAT32_VERSION = 42,
    ROOT_DIR_CLUSTER = 44,
    FSINFO_SECTOR = 48,
    BACKUP_BOOT_SECTOR_ADDR = 50,
    FAT32_BIOS_DRIVE_NUMBER = 64,
    FAT32_EXTENDED_BOOT_SIG = 66,
    FAT32_VOLUME_SERIAL= 67,
    FAT32_VOLUME_LABEL =  71,
    FAT32_FS_TYPE_LABEL = 82,

    // FAT Directory Entry
    ALLOCATION_STATUS = 0,
    FILE_NAME = 0,
    FILE_ATTRIBUTES = 11,
    CREATED_TIME_TENTHS = 13,
    CREATED_TIME_HMS = 14,
    CREATED_DAY = 16,
    ACCESSED_DAY = 18,
    HIGH_CLUSTER_ADDR = 20,
    WRITTEN_TIME_HMS = 22,
    WRITTEN_DAY = 24,
    LOW_CLUSTER_ADDR = 26,
    FILE_SIZE = 28,


    // FAT Flag Values
    FLAG_FAT_READ_ONLY = 0x1,
    FLAG_FAT_HIDDEN_FILE = 0x2,
    FLAG_FAT_SYSTEM_FILE = 0x3,
    FLAG_FAT_VOLUME_LABEL = 0x8,
    FLAG_FAT_LONG_FILE_NAME = 0x0F,
    FLAG_FAT_DIRECTORY = 0x10,
    FLAG_FAT_ARCHIVE = 0x20
};

enum media_types{
    REMOVABLE = 0xf0,
    FIXED = 0xf8
};

/**
 * @brief 
 */
enum signatures {
    MBR_SIG = 0x55AA,
    NTFS_SIG = 0xEB5290,
    FAT12_SIG = 0xEB3F90,
    FAT16_SIG = 0xEB3C90,
    FAT32_SIG = 0xEB5890
};

/**
 * @brief 
 */
enum eof {
    FAT12_EOF = 0xff8,
    FAT16_EOF = 0xfff8,
    FAT32_EOF = 0x0ffffff8
};

/**
 * @brief 
 */
enum bad_sector {
    FAT12_BAD = 0xff7,
    FAT16_BAD = 0xfff7,
    FAT32_BAD = 0x0ffffff7
};

/**
 * @brief 
 */
enum file_allocation_status {
    UNALLOCATED = 0xe5
};

// Struct to store command line args
typedef struct cmd_line {
    // Booleans to specify if flag was present
    bool i_flag; // disk image path flag
    bool f_flag; // file system format flag
    bool v_flag; // verbose flag
    bool h_flag; // hidden flag

    // Flag values
    char argv0[255];
    char image_path[255];
    char file_system[8];
    int fs_type;
} cmd_line;


// Reference for MBR and EBR data structure and offsets:
// https://thestarman.pcministry.com/asm/mbr/PartTables.htm
typedef struct ebr_table {
    uint32_t offset; // The lba of this ebr_entry   
    uint32_t starting_sector; // add offset + starting sector to find first block of partition
    uint32_t partition_size;  // size in sectors
    uint32_t next_partition_ebr;
    struct ebr_table *next_ebr_table;
} ebr_table;

// Struct to store MBR fields
typedef struct partition_table {
    // Booleans to specify if flag was present
    uint8_t boot_indicator;
    uint8_t partition_type;
    uint32_t starting_sector;
    uint32_t partition_size;  // size in sectors
    struct ebr_table *ebr_table; // will be NULL if partition is not extended
} partition_table;

// Struct to store array of MBR Table Entries
typedef struct mbr_sector {
    struct partition_table entry[4];
} mbr_sector;

// Struct to store FAT Boot Sector fields
typedef struct fat_boot_sector {
    bool is_fat32;
    bool is_fat16;
    bool is_fat12;
    
    char oem_name[9];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_area_size; // size is in sectors
    uint8_t number_of_fats;
    uint16_t max_files_in_root;
    uint16_t sector_count_16b;
    uint8_t media_type;
    uint16_t fat_size_in_sectors;  // used for FAT12/16, will be 0 in FAT32
    uint16_t sectors_per_track;
    uint16_t head_number;
    uint32_t sectors_before_partition;
    uint32_t sector_count_32b; // if this is set, sector_count must be 0
    uint8_t bios_drive_number;
    uint8_t extended_boot_sig;
    uint32_t volume_serial;
    char volume_label[12];
    char fs_type_label[9]; // not required, but can be FAT, FAT12, FAT16
    uint16_t fs_signature;

    // The below fields only apply to FAT32
    uint32_t fat32_size_in_sectors;
    uint16_t fat_mode;
    uint16_t fat32_version;
    uint32_t root_dir_cluster;
    uint16_t fsinfo_sector_addr;
    uint16_t backup_boot_sector_addr;
    uint8_t fat32_bios_drive_number;
    uint8_t fat32_extended_boot_sig;
    uint32_t fat32_volume_serial;
    char fat32_volume_label[12];
    char fat32_fs_type_label[9];

} fat_boot_sector;

typedef struct fat_dir_entry{
    bool is_directory;
    union {
        char alloc_status;
        char filename[12];
    } info;
    uint8_t file_attributes;
    uint8_t created_time_tenths;
    uint16_t created_time_hms;
    uint16_t created_day;
    uint16_t accessed_day;
    uint32_t low_cluster_addr;
    uint32_t high_cluster_addr;
    uint32_t cluster_addr; // generated by combining low and high cluster addr
    uint16_t written_time_hms;
    uint16_t written_day;
    uint32_t file_size; // in bytes
    uint32_t last_cluster; // Store the last cluster of the file/dir for feeler gauge checks

    // Linked List of parent
    struct fat_dir_entry* parent_dir;

    // Linked List to files and subfolders
    struct fat_dir_entry* dir_contents;

    // Double linked list of all files/folders within the same directory
    struct fat_dir_entry* next;
     struct fat_dir_entry* prev;

} fat_dir_entry;

typedef struct read_parameters{
    uint32_t start_cluster; // cluster where the file/data to be read begins
    uint32_t *cluster_list; // list of clusters that contain the other segments of the file
    uint32_t list_length; // # of clusters in the list
    uint32_t entry_offset; // offset within the custer to begin reading (used for directory entries)
} read_parameters;

/**
 * @brief Lookup table for partition code -> txt string
 */
const char partition_type_txt [256][20]={
    "EMPTY",            // [0] -> 0x00
    "FAT12",            // [1] -> 0x01
    "XENIX ROOT",       // [2] -> 0x2
    "XENIX USR",        // [3] -> 0x3
    "FAT16 (CHS)",      // [4] -> 0x4
    "EXTENDED (CHS)",   // [5] -> 0x5
    "FAT16B (CHS)",     // [6] -> 0x6
    "NTFS",             // [7] -> 0x7
    "IBM AIX",          // [8] -> 0x8
    "IBM AIX",          // [9] -> 0x9
    "IBM OS/2",         // [10] -> 0xA
    "FAT32 (CHS)",      // [11] -> 0xB
    "FAT32 (LBA)",      // [12] -> 0xC
    "????",             // [13] -> 0xD
    "FAT16 (LBA)",      // [14] -> 0xE
    "EXTENDED (LBA)",   // [15] -> 0xF
    "Hidden IBM OS/2",  // [16] -> 0x10
    "Hidden NTFS",      // [17] -> 0x11
    "????",             // [18] -> 0x12
    "????",             // [19] -> 0x13
    "Hidden FAT16",     // [20] -> 0x14
    "????",             // [21] -> 0x15
    "Hidden FAT16",     // [22] -> 0x16
    "????",             // [23] -> 0x17
    "????",             // [24] -> 0x18
    "????",             // [25] -> 0x19
    "????",             // [26] -> 0x1A
    "Hidden FAT32",     // [27] -> 0x1B
    "Hidden FAT32",     // [28] -> 0x1C
    "????",             // [29] -> 0x1D
    "Hidden FAT16",     // [30] -> 0x1E
    "????",             // [31] -> 0x1F
    "????",             // [32] -> 0x20
    "????",             // [33] -> 0x21
    "????",             // [34] -> 0x22
    "????",             // [35] -> 0x23
    "????",             // [36] -> 0x24
    "????",             // [37] -> 0x25
    "????",             // [38] -> 0x26
    "????",             // [39] -> 0x27
    "????",             // [40] -> 0x28
    "????",             // [41] -> 0x29
    "MBR Dynamic",      // [42] -> 0x2A
    "????",             // [43] -> 0x2B
    "????",             // [44] -> 0x2C
    "????",             // [45] -> 0x2D
    "????",             // [46] -> 0x2E
    "????",             // [47] -> 0x2F
    "????",             // [48] -> 0x30
    "????",             // [49] -> 0x31
    "????",             // [50] -> 0x32
    "????",             // [51] -> 0x33
    "????",             // [52] -> 0x34
    "????",             // [53] -> 0x35
    "????",             // [54] -> 0x36
    "????",             // [55] -> 0x37
    "????",             // [56] -> 0x38
    "????",             // [57] -> 0x39
    "????",             // [58] -> 0x3A
    "????",             // [59] -> 0x3B
    "????",             // [60] -> 0x3C
    "????",             // [61] -> 0x3D
    "????",             // [62] -> 0x3E
    "????",             // [63] -> 0x3F
    "????",             // [64] -> 0x40
    "????",             // [65] -> 0x41
    "????",             // [66] -> 0x42
    "????",             // [67] -> 0x43
    "????",             // [68] -> 0x44
    "????",             // [69] -> 0x45
    "????",             // [70] -> 0x46
    "????",             // [71] -> 0x47
    "????",             // [72] -> 0x48
    "????",             // [73] -> 0x49
    "????",             // [74] -> 0x4A
    "????",             // [75] -> 0x4B
    "????",             // [76] -> 0x4C
    "????",             // [77] -> 0x4D
    "????",             // [78] -> 0x4E
    "????",             // [79] -> 0x4F
    "????",             // [80] -> 0x50
    "????",             // [81] -> 0x51
    "????",             // [82] -> 0x52
    "????",             // [83] -> 0x53
    "????",             // [84] -> 0x54
    "????",             // [85] -> 0x55
    "????",             // [86] -> 0x56
    "????",             // [87] -> 0x57
    "????",             // [88] -> 0x58
    "????",             // [89] -> 0x59
    "????",             // [90] -> 0x5A
    "????",             // [91] -> 0x5B
    "????",             // [92] -> 0x5C
    "????",             // [93] -> 0x5D
    "????",             // [94] -> 0x5E
    "????",             // [95] -> 0x5F
    "????",             // [96] -> 0x60
    "????",             // [97] -> 0x61
    "????",             // [98] -> 0x62
    "????",             // [99] -> 0x63
    "????",             // [100] -> 0x64
    "????",             // [101] -> 0x65
    "????",             // [102] -> 0x66
    "????",             // [103] -> 0x67
    "????",             // [104] -> 0x68
    "????",             // [105] -> 0x69
    "????",             // [106] -> 0x6A
    "????",             // [107] -> 0x6B
    "????",             // [108] -> 0x6C
    "????",             // [109] -> 0x6D
    "????",             // [110] -> 0x6E
    "????",             // [111] -> 0x6F
    "????",             // [112] -> 0x70
    "????",             // [113] -> 0x71
    "????",             // [114] -> 0x72
    "????",             // [115] -> 0x73
    "????",             // [116] -> 0x74
    "????",             // [117] -> 0x75
    "????",             // [118] -> 0x76
    "????",             // [119] -> 0x77
    "????",             // [120] -> 0x78
    "????",             // [121] -> 0x79
    "????",             // [122] -> 0x7A
    "????",             // [123] -> 0x7B
    "????",             // [124] -> 0x7C
    "????",             // [125] -> 0x7D
    "????",             // [126] -> 0x7E
    "????",             // [127] -> 0x7F
    "????",             // [128] -> 0x80
    "????",             // [129] -> 0x81
    "Linux Swap",       // [130] -> 0x82
    "Linux",            // [131] -> 0x83
    "Hibernation",      // [132] -> 0x84
    "EXTENDED (Linux)", // [133] -> 0x85
    "NTFS Vol Set",     // [134] -> 0x86
    "NTFS Vol Set",     // [135] -> 0x87
    "????",             // [136] -> 0x88
    "????",             // [137] -> 0x89
    "????",             // [138] -> 0x8A
    "????",             // [139] -> 0x8B
    "????",             // [140] -> 0x8C
    "????",             // [141] -> 0x8D
    "????",             // [142] -> 0x8E
    "????",             // [143] -> 0x8F
    "????",             // [144] -> 0x90
    "????",             // [145] -> 0x91
    "????",             // [146] -> 0x92
    "????",             // [147] -> 0x93
    "????",             // [148] -> 0x94
    "????",             // [149] -> 0x95
    "????",             // [150] -> 0x96
    "????",             // [151] -> 0x97
    "????",             // [152] -> 0x98
    "????",             // [153] -> 0x99
    "????",             // [154] -> 0x9A
    "????",             // [155] -> 0x9B
    "????",             // [156] -> 0x9C
    "????",             // [157] -> 0x9D
    "????",             // [158] -> 0x9E
    "????",             // [159] -> 0x9F
    "Hibernation",      // [160] -> 0xA0
    "Hibernation",      // [161] -> 0xA1
    "????",             // [162] -> 0xA2
    "????",             // [163] -> 0xA3
    "????",             // [164] -> 0xA4
    "FreeBSD",          // [165] -> 0xA5
    "OpenBSD",          // [166] -> 0xA6
    "????",             // [167] -> 0xA7
    "MacOS X",          // [168] -> 0xA8
    "NetBSD",           // [169] -> 0xA9
    "????",             // [170] -> 0xAA
    "MacOS X Boot",     // [171] -> 0xAB
    "????",             // [172] -> 0xAC
    "????",             // [173] -> 0xAD
    "????",             // [174] -> 0xAE
    "????",             // [175] -> 0xAF
    "????",             // [176] -> 0xB0
    "????",             // [177] -> 0xB1
    "????",             // [178] -> 0xB2
    "????",             // [179] -> 0xB3
    "????",             // [180] -> 0xB4
    "????",             // [181] -> 0xB5
    "????",             // [182] -> 0xB6
    "????",             // [183] -> 0xB7
    "????",             // [184] -> 0xB8
    "????",             // [185] -> 0xB9
    "????",             // [186] -> 0xBA
    "????",             // [187] -> 0xBB
    "????",             // [188] -> 0xBC
    "????",             // [189] -> 0xBD
    "????",             // [190] -> 0xBE
    "????",             // [191] -> 0xBF
    "????",             // [192] -> 0xC0
    "????",             // [193] -> 0xC1
    "????",             // [194] -> 0xC2
    "????",             // [195] -> 0xC3
    "????",             // [196] -> 0xC4
    "????",             // [197] -> 0xC5
    "????",             // [198] -> 0xC6
    "????",             // [199] -> 0xC7
    "????",             // [200] -> 0xC8
    "????",             // [201] -> 0xC9
    "????",             // [202] -> 0xCA
    "????",             // [203] -> 0xCB
    "????",             // [204] -> 0xCC
    "????",             // [205] -> 0xCD
    "????",             // [206] -> 0xCE
    "????",             // [207] -> 0xCF
    "????",             // [208] -> 0xD0
    "????",             // [209] -> 0xD1
    "????",             // [210] -> 0xD2
    "????",             // [211] -> 0xD3
    "????",             // [212] -> 0xD4
    "????",             // [213] -> 0xD5
    "????",             // [214] -> 0xD6
    "????",             // [215] -> 0xD7
    "????",             // [216] -> 0xD8
    "????",             // [217] -> 0xD9
    "????",             // [218] -> 0xDA
    "????",             // [219] -> 0xDB
    "????",             // [220] -> 0xDC
    "????",             // [221] -> 0xDD
    "????",             // [222] -> 0xDE
    "????",             // [223] -> 0xDF
    "????",             // [224] -> 0xE0
    "????",             // [225] -> 0xE1
    "????",             // [226] -> 0xE2
    "????",             // [227] -> 0xE3
    "????",             // [228] -> 0xE4
    "????",             // [229] -> 0xE5
    "????",             // [230] -> 0xE6
    "????",             // [231] -> 0xE7
    "????",             // [232] -> 0xE8
    "????",             // [233] -> 0xE9
    "????",             // [234] -> 0xEA
    "????",             // [235] -> 0xEB
    "????",             // [236] -> 0xEC
    "????",             // [237] -> 0xED
    "????",             // [238] -> 0xEE
    "????",             // [239] -> 0xEF
    "????",             // [240] -> 0xF0
    "????",             // [241] -> 0xF1
    "????",             // [242] -> 0xF2
    "????",             // [243] -> 0xF3
    "????",             // [244] -> 0xF4
    "????",             // [245] -> 0xF5
    "????",             // [246] -> 0xF6
    "????",             // [247] -> 0xF7
    "????",             // [248] -> 0xF8
    "????",             // [249] -> 0xF9
    "????",             // [250] -> 0xFA
    "????",             // [251] -> 0xFB
    "????",             // [252] -> 0xFC
    "????",             // [253] -> 0xFD
    "????",             // [254] -> 0xFE
    "????"              // [255] -> 0xFF
}; 