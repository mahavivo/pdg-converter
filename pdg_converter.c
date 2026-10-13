/* PDG Converter, native C / Windows. UTF-8 source, C11.
   CCITT tables: pdfminer.six, MIT; see licenses/pdfminer_CCITT_LICENSE.txt.
   MD5 and image codecs use Windows BCrypt and WIC. No Python runtime. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <oleauto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <time.h>
#include "ccitt_tables.h"

/* ==================== Edit these settings in your IDE ==================== */
static const wchar_t *INPUT_DIR = L"input";
/* Relative output paths resolve beside the EXE, independently of IDE cwd. */
static const wchar_t *OUTPUT_DIR = L"converted_images";
static int RECURSIVE = 1;
static int OVERWRITE_EXISTING = 0;
static int SHOW_SUMMARY = 0;                   /* 1: print final statistics */
static int WRITE_REPORTS = 0;                  /* 1: create CSV and JSON reports */
static const wchar_t *OUTPUT_FORMAT = L"auto"; /* auto / png / tiff / bmp / jpg */
static double IMAGE_DPI = 300.0;
static float JPEG_QUALITY = 0.95f;
static uint64_t MAX_FILE_BYTES = 256ULL * 1024 * 1024;
static uint64_t MAX_IMAGE_PIXELS = 100000000ULL;
/* ======================================================================== */

#define PATH_CAP 32768
#define ERROR_CAP 1024
#define MODE_PASS 100
#define MODE_HORIZONTAL 101

typedef enum
{
    F_UNKNOWN,
    F_FAX,
    F_PNG,
    F_JPEG,
    F_TIFF,
    F_BMP,
    F_GIF,
    F_WEBP,
    F_JP2,
    F_J2K,
    F_DJVU
} Format;

/* data 指向 file 内的载荷区域，不单独释放；pixels 是解码后的独立像素缓冲。 */
typedef struct
{
    uint8_t *file;
    uint8_t *data;
    uint8_t *pixels;
    size_t file_size;
    size_t size;
    size_t stride;
    unsigned w;
    unsigned h;
    unsigned version;
    unsigned kind;
    unsigned frames;
    Format format;
} Page;

typedef struct
{
    uint8_t *data;
    size_t size;
    size_t bit;
} Reader;

typedef struct
{
    wchar_t *path;
    wchar_t *relative;
} Entry;

typedef struct
{
    Entry *items;
    size_t size;
    size_t cap;
} Entries;

static char error_text[ERROR_CAP];
static IWICImagingFactory *factory;
static FILE *csv_report;
static FILE *json_report;
static int report_first = 1;
static size_t successes;
static size_t skipped;
static size_t failures;

/* 基础工具：错误信息、字节序、字符串、路径与文件读取 */
static int fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_text, ERROR_CAP, fmt, args);
    va_end(args);
    return 0;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

static void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static double now_seconds(void)
{
    LARGE_INTEGER a, f;
    QueryPerformanceCounter(&a);
    QueryPerformanceFrequency(&f);
    return (double)a.QuadPart / (double)f.QuadPart;
}

static char *utf8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = (char *)malloc((size_t)n);
    if (out)
    {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    }
    return out;
}

static wchar_t *duplicate(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *p = (wchar_t *)malloc(n);
    if (p)
    {
        memcpy(p, s, n);
    }
    return p;
}

static wchar_t *join(const wchar_t *a, const wchar_t *b)
{
    size_t n = wcslen(a), m = wcslen(b);
    if (n + m + 2 >= PATH_CAP)
    {
        return NULL;
    }
    wchar_t *p = (wchar_t *)malloc((n + m + 2) * sizeof(wchar_t));
    if (!p)
    {
        return NULL;
    }
    wcscpy(p, a);
    if (n && p[n - 1] != L'\\' && p[n - 1] != L'/')
    {
        p[n++] = L'\\';
    }
    wcscpy(p + n, b);
    return p;
}

static int exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int mkdirs(const wchar_t *path)
{
    wchar_t *p = duplicate(path);
    size_t n;
    if (!p)
    {
        return fail("Out of memory");
    }
    for (n = 3; p[n]; n++)
    {
        if (p[n] == L'\\' || p[n] == L'/')
        {
            wchar_t c = p[n];
            p[n] = 0;
            if (!CreateDirectoryW(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                free(p);
                return fail("Cannot create output directory (Windows error %lu)", GetLastError());
            }
            p[n] = c;
        }
    }
    if (!CreateDirectoryW(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        free(p);
        return fail("Cannot create directory (Windows error %lu)", GetLastError());
    }
    free(p);
    return 1;
}

static int parent_dirs(const wchar_t *file)
{
    wchar_t *p = duplicate(file), *s;
    if (!p)
    {
        return fail("Out of memory");
    }
    s = wcsrchr(p, L'\\');
    if (s)
    {
        *s = 0;
    }
    int result = s ? mkdirs(p) : 1;
    free(p);
    return result;
}

static int path_inside(const wchar_t *path, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    return !_wcsnicmp(path, dir, n) && (path[n] == 0 || path[n] == L'\\' || dir[n - 1] == L'\\');
}

static int read_file(const wchar_t *path, Page *page)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER length;
    DWORD got;
    if (file == INVALID_HANDLE_VALUE)
    {
        return fail("Cannot read input (Windows error %lu)", GetLastError());
    }
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
        (uint64_t)length.QuadPart > MAX_FILE_BYTES)
    {
        CloseHandle(file);
        return fail("Empty input or file exceeds size limit");
    }
    page->file_size = (size_t)length.QuadPart;
    page->file = (uint8_t *)malloc(page->file_size);
    if (!page->file)
    {
        CloseHandle(file);
        return fail("Out of memory");
    }
    if (!ReadFile(file, page->file, (DWORD)page->file_size, &got, NULL) || got != page->file_size)
    {
        CloseHandle(file);
        return fail("Input truncated during read");
    }
    CloseHandle(file);
    page->data = page->file;
    page->size = page->file_size;
    return 1;
}

static void free_page(Page *p)
{
    free(p->pixels);
    free(p->file);
    memset(p, 0, sizeof(*p));
}

/* 图像容器：按签名识别格式，检查并裁剪容器范围 */
static Format detect(const uint8_t *p, size_t n)
{
    if (n >= 3 && p[0] == 255 && p[1] == 216 && p[2] == 255)
    {
        return F_JPEG;
    }
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
    {
        return F_PNG;
    }
    if (n >= 4 && (!memcmp(p, "II*\0", 4) || !memcmp(p, "MM\0*", 4) || !memcmp(p, "II+\0", 4) ||
                   !memcmp(p, "MM\0+", 4)))
    {
        return F_TIFF;
    }
    if (n >= 2 && !memcmp(p, "BM", 2))
    {
        return F_BMP;
    }
    if (n >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6)))
    {
        return F_GIF;
    }
    if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4))
    {
        return F_WEBP;
    }
    if (n >= 12 && !memcmp(p, "\0\0\0\x0cjP  \r\n\x87\n", 12))
    {
        return F_JP2;
    }
    if (n >= 4 && !memcmp(p, "\xffO\xffQ", 4))
    {
        return F_J2K;
    }
    if (n >= 8 && !memcmp(p, "AT&TFORM", 8))
    {
        return F_DJVU;
    }
    return F_UNKNOWN;
}

static const wchar_t *extension(Format f)
{
    switch (f)
    {
        case F_PNG:
            return L".png";
        case F_JPEG:
            return L".jpg";
        case F_TIFF:
            return L".tiff";
        case F_BMP:
            return L".bmp";
        case F_GIF:
            return L".gif";
        case F_WEBP:
            return L".webp";
        case F_JP2:
            return L".jp2";
        case F_J2K:
            return L".j2k";
        case F_DJVU:
            return L".djvu";
        default:
            return L".png";
    }
}

static const char *format_name(Format f)
{
    static const char *names[] = {"unknown", "FAX",  "PNG",      "JPEG", "TIFF", "BMP",
                                  "GIF",     "WebP", "JPEG2000", "J2K",  "DjVu"};
    return names[f];
}

static int trim(Page *page)
{
    const uint8_t *p = page->data;
    size_t n = page->size, pos, end;
    uint32_t size;
    unsigned marker;
    switch (page->format)
    {
        case F_PNG:
            pos = 8;
            while (pos + 12 <= n)
            {
                size = be32(p + pos);
                if ((size_t)size > n - pos - 12)
                {
                    return fail("Truncated PNG");
                }
                end = pos + 12 + size;
                if (!memcmp(p + pos + 4, "IEND", 4))
                {
                    if (size)
                    {
                        return fail("Invalid PNG IEND");
                    }
                    page->size = end;
                    return 1;
                }
                pos = end;
            }
            return fail("PNG has no IEND");
        case F_JPEG:
            pos = 2;
            while (pos < n)
            {
                if (p[pos] != 255)
                {
                    return fail("Invalid JPEG marker");
                }
                while (pos < n && p[pos] == 255)
                {
                    pos++;
                }
                if (pos >= n)
                {
                    break;
                }
                marker = p[pos++];
                if (marker == 217)
                {
                    page->size = pos;
                    return 1;
                }
                if (marker == 1 || (marker >= 208 && marker <= 216))
                {
                    continue;
                }
                if (!marker || pos + 2 > n)
                {
                    return fail("Invalid JPEG");
                }
                size = (uint32_t)(p[pos] << 8 | p[pos + 1]);
                if (size < 2 || size > n - pos)
                {
                    return fail("Truncated JPEG");
                }
                pos += size;
                if (marker == 218)
                {
                    for (;;)
                    {
                        while (pos < n && p[pos] != 255)
                        {
                            pos++;
                        }
                        if (pos >= n)
                        {
                            return fail("JPEG has no EOI");
                        }
                        end = pos + 1;
                        while (end < n && p[end] == 255)
                        {
                            end++;
                        }
                        if (end >= n)
                        {
                            return fail("Truncated JPEG scan");
                        }
                        marker = p[end];
                        if (!marker || (marker >= 208 && marker <= 215))
                        {
                            pos = end + 1;
                            continue;
                        }
                        break;
                    }
                }
            }
            return fail("JPEG has no EOI");
        case F_BMP:
            if (n < 14)
            {
                return fail("Truncated BMP");
            }
            end = le32(p + 2);
            if (end < 14 || end > n)
            {
                return fail("Invalid BMP size");
            }
            page->size = end;
            break;
        case F_WEBP:
            end = (size_t)le32(p + 4) + 8;
            if (end < 12 || end > n)
            {
                return fail("Invalid WebP size");
            }
            page->size = end;
            break;
        case F_DJVU:
            if (n < 16 || (memcmp(p + 12, "DJVU", 4) && memcmp(p + 12, "DJVM", 4)))
            {
                return fail("Unsupported DjVu FORM type");
            }
            end = (size_t)be32(p + 8) + 12;
            if (end < 16 || end > n)
            {
                return fail("Invalid DjVu size");
            }
            page->size = end;
            break;
        default:
            break;
    }
    return 1;
}

/* PDG 数据恢复：密钥派生、分组逆变换与稀疏字节扰码 */
static int md5(const uint8_t *data, size_t n, uint8_t key[16])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS result = BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0);
    if (result >= 0)
    {
        result = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    }
    if (result >= 0)
    {
        result = BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0);
    }
    if (result >= 0)
    {
        result = BCryptFinishHash(hash, key, 16, 0);
    }
    if (hash)
    {
        BCryptDestroyHash(hash);
    }
    if (alg)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result >= 0 ? 1 : fail("Windows MD5 operation failed (0x%08lx)", (unsigned long)result);
}

static void decrypt_blocks(uint8_t *data, size_t size, const uint8_t key[16])
{
    uint32_t k0 = le32(key);
    uint32_t k1 = le32(key + 4);
    uint32_t k2 = le32(key + 8);
    uint32_t k3 = le32(key + 12);
    /* 每个 16 字节块独立进行 16 轮逆变换，所有字运算自然按 uint32_t 溢出。 */
    for (size_t i = 0; i + 16 <= size; i += 16)
    {
        uint32_t a = le32(data + i);
        uint32_t b = le32(data + i + 4);
        uint32_t c = le32(data + i + 8);
        uint32_t d = le32(data + i + 12);
        uint32_t sum = 0xe3779b90u;
        for (int r = 0; r < 16; r++)
        {
            d -= ((a >> 5) + k1) ^ ((a << 4) + k2) ^ (a + sum);
            c -= ((d << 4) + k0) ^ ((d >> 5) + k3) ^ (d + sum);
            b -= ((c >> 5) + k3) ^ ((c << 4) + k2) ^ (c + sum);
            a -= ((b << 4) + k0) ^ ((b >> 5) + k1) ^ (b + sum);
            sum -= 0x9e3779b9u;
        }
        put32(data + i, a);
        put32(data + i + 4, b);
        put32(data + i + 8, c);
        put32(data + i + 12, d);
    }
}

static int restore(Page *p)
{
    if (p->kind == 0)
    {
        return 1;
    }
    if (p->kind == 1)
    {
        const char *key = "3.141592";
        for (size_t i = 0; i < p->size / 8 * 8; i++)
        {
            p->data[i] ^= (uint8_t)key[i & 7];
        }
        return 1;
    }
    if (p->kind == 17)
    {
        if (le32(p->file + 8) != 12)
        {
            return fail("Unsupported type 0x11 metadata layout");
        }
        /* 位置相对于载荷起点；位掩码对应原算法的模 256 / 模 512 运算。 */
        uint32_t a = p->w & 255;
        uint32_t d = (p->w - p->h) & 255;
        uint32_t s = (p->w * p->h) & 255;
        uint32_t b = ((uint32_t)p->size >> 8) & 255;
        uint32_t q = (b * p->w) & 255;
        uint32_t t = (p->w | p->h) & 255;
        uint32_t n = (p->w + p->h + (uint32_t)p->size) & 255;
        uint32_t e = (s * a + le16(p->file + 0x5e)) & 255;
        uint32_t i = (e + q * b + t * s - n * d + 250) & 511;
        uint32_t j = (t * t + n * d * s - e * q + 250) & 511;
        if (i > j)
        {
            uint32_t tmp = i;
            i = j;
            j = tmp;
        }
        if (i == j)
        {
            return 1;
        }
        unsigned step = (j - i == 17) ? 19 : 17;
        if (j + step >= p->size)
        {
            return fail("Type 0x11 payload too short");
        }
        uint8_t old = p->data[i];
        p->data[i] = p->data[j] ^ p->data[j + step];
        p->data[j] = old ^ p->data[i + step];
        return 1;
    }
    if (p->kind >= 2 && p->kind <= 5)
    {
        uint8_t material[48], key[16];
        memcpy(material, p->file + 0x40, 48);
        if (p->kind > 2)
        {
            material[0x26] = 0;
        }
        if (!md5(material, 48, key))
        {
            return 0;
        }
        const char *mask = p->kind == 3   ? "SUPERSTAR4PDG2.0"
                           : p->kind == 4 ? "SSREADER"
                           : p->kind == 5 ? "e#fgF%3*"
                                          : NULL;
        if (mask)
        {
            for (int i = 0; i < 8; i++)
            {
                key[i] ^= (uint8_t)mask[i];
            }
        }
        decrypt_blocks(p->data, p->size, key);
        return 1;
    }
    return fail("Unsupported PDG encoding 0x%02x", p->kind);
}

static int dimensions(Page *p)
{
    return p->w && p->h && (uint64_t)p->w * p->h <= MAX_IMAGE_PIXELS
               ? 1
               : fail("Invalid dimensions or image exceeds pixel limit (%u x %u)", p->w, p->h);
}

static int prepare(Page *p)
{
    p->format = detect(p->data, p->size);
    if (p->format != F_UNKNOWN)
    {
        return trim(p);
    }
    if (p->size < 3 || memcmp(p->data, "HH", 2))
    {
        return fail("Not an HH PDG or recognizable image");
    }
    p->version = p->file[2];
    if (p->version == 1)
    {
        if (p->size < 56)
        {
            return fail("Truncated v1 header");
        }
        p->w = le16(p->file + 3);
        p->h = le16(p->file + 5);
        p->kind = le32(p->file + 7);
        p->data = p->file + 56;
        p->size -= 56;
        if (p->kind != 0 && p->kind != 4 && p->kind != 5)
        {
            return fail("Unsupported v1 type %u", p->kind);
        }
        if (p->kind == 4 || p->kind == 5)
        {
            size_t count = p->kind == 4 ? 1 : 2;
            if (p->file_size < 189)
            {
                return fail("Truncated v1 payload");
            }
            memmove(p->data + 128, p->data + 128 + count, p->size - 128 - count);
            p->size -= count;
        }
    }
    else if (p->version == 2)
    {
        if (p->size < 128)
        {
            return fail("Truncated v2 header");
        }
        if (p->file[0x38])
        {
            return fail("Unsupported outer transformation at 0x38");
        }
        if (p->file[0x15])
        {
            return fail("Additional image layers are not implemented");
        }
        p->w = le16(p->file + 16);
        p->h = le16(p->file + 18);
        p->kind = p->file[15];
        uint32_t offset = le32(p->file + 24), length = le32(p->file + 28);
        if (offset < 128 || !length || offset > p->file_size || length > p->file_size - offset)
        {
            return fail("Invalid payload bounds");
        }
        p->data = p->file + offset;
        p->size = length;
        p->format = detect(p->data, p->size);
        if (p->format != F_UNKNOWN)
        {
            return trim(p);
        }
        if (!restore(p))
        {
            return 0;
        }
    }
    else
    {
        return fail("Unsupported PDG version %u", p->version);
    }
    p->format = detect(p->data, p->size);
    if (p->format != F_UNKNOWN)
    {
        return trim(p);
    }
    if (p->version == 2 && (p->file[14] || p->kind == 4))
    {
        return fail("Unrecognized image payload; refusing to guess fax format");
    }
    p->format = F_FAX;
    return dimensions(p);
}

/* 传真解码：读取模式和游程，按参考行变化点还原像素 */
static int read_code(Reader *r, const CodeNode *tree)
{
    int node = 0;
    while (tree[node].child[0] >= 0)
    {
        if (r->bit >= r->size * 8)
        {
            return 9999;
        }
        unsigned bit = (r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
        r->bit++;
        node = tree[node].child[bit];
    }
    return tree[node].value;
}

static int run_length(Reader *r, int white)
{
    int total = 0;
    for (;;)
    {
        int x = read_code(r, white ? white_tree : black_tree);
        if (x < 0 || x == 9999)
        {
            return -1;
        }
        total += x;
        if (total > 131072)
        {
            return -1;
        }
        if (x < 64)
        {
            return total;
        }
    }
}

static void white_range(uint8_t *row, unsigned a, unsigned b)
{
    if (b <= a)
    {
        return;
    }
    unsigned first = a >> 3, last = (b - 1) >> 3;
    uint8_t left = (uint8_t)(255u >> (a & 7));
    uint8_t right = (uint8_t)(255u << (7 - ((b - 1) & 7)));
    if (first == last)
    {
        row[first] |= left & right;
        return;
    }
    row[first] |= left;
    if (last > first + 1)
    {
        memset(row + first + 1, 255, last - first - 1);
    }
    row[last] |= right;
}

static int decode_fax(Page *p)
{
    if (!dimensions(p))
    {
        return 0;
    }
    size_t cap = (size_t)p->w * 4 + 1030;
    p->stride = (p->w + 7) / 8;
    p->pixels = (uint8_t *)calloc(p->h, p->stride);
    /* 参考行从位置 0 的黑色状态开始，不能直接套用普通白色初始状态的 G4 解码。 */
    int *previous = (int *)malloc(cap * sizeof(int)), *current = (int *)malloc(cap * sizeof(int));
    int result = 0;
    if (!previous || !current || !p->pixels)
    {
        fail("Out of memory in fax decoder");
        goto done;
    }
    for (int i = 0; i < 5; i++)
    {
        previous[i] = i ? p->w : 0;
    }
    size_t prev_count = 5;
    Reader reader = {p->data, p->size, 0};
    for (unsigned y = 0; y < p->h; y++)
    {
        size_t ref = 0, count = 0, operations = 0;
        int pos = 0, white = 0;
        while (pos < (int)p->w)
        {
            if (++operations > cap - 5)
            {
                fail("Fax operation limit at row %u", y + 1);
                goto done;
            }
            int mode = read_code(&reader, mode_tree);
            if (mode == MODE_PASS)
            {
                if (ref + 1 >= prev_count)
                {
                    fail("Fax reference bounds at row %u", y + 1);
                    goto done;
                }
                pos = previous[++ref];
                ref++;
            }
            else if (mode == MODE_HORIZONTAL)
            {
                int a = run_length(&reader, white), b = run_length(&reader, !white);
                if (a < 0 || b < 0)
                {
                    fail("Invalid/truncated fax run at row %u", y + 1);
                    goto done;
                }
                pos += a;
                current[count++] = pos;
                pos += b;
                current[count++] = pos;
                while (ref < prev_count && previous[ref] <= pos)
                {
                    ref += 2;
                }
            }
            else if (mode >= -3 && mode <= 3)
            {
                if (ref >= prev_count)
                {
                    fail("Fax reference bounds at row %u", y + 1);
                    goto done;
                }
                pos = previous[ref++] + mode;
                if (pos < 0)
                {
                    pos = 0;
                }
                current[count++] = pos;
                white = !white;
            }
            else
            {
                fail("Unsupported or truncated fax mode at row %u (bit %zu)", y + 1, reader.bit);
                goto done;
            }
            if (count + 4 >= cap)
            {
                fail("Fax change list overflow");
                goto done;
            }
        }
        uint8_t *row = p->pixels + (size_t)y * p->stride;
        for (size_t i = 0; i + 1 < count; i += 2)
        {
            int a = current[i], b = current[i + 1];
            if (a < 0)
            {
                a = 0;
            }
            if (b < 0)
            {
                b = 0;
            }
            if (a > (int)p->w)
            {
                a = p->w;
            }
            if (b > (int)p->w)
            {
                b = p->w;
            }
            if (b > a)
            {
                white_range(row, (unsigned)a, (unsigned)b);
            }
        }
        /* 追加哨兵变化点，再交换当前行和上一行缓冲。 */
        for (int i = 0; i < 4; i++)
        {
            current[count + i] = pos;
        }
        prev_count = count + 4;
        int *swap = previous;
        previous = current;
        current = swap;
    }
    result = 1;
done:
    free(previous);
    free(current);
    return result;
}

/* Windows WIC：格式选择、标准图像校验与图像编码 */
static Format output_format(Page *p)
{
    if (!_wcsicmp(OUTPUT_FORMAT, L"auto"))
    {
        return p->format == F_FAX ? F_PNG : p->format;
    }
    if (!_wcsicmp(OUTPUT_FORMAT, L"png"))
    {
        return F_PNG;
    }
    if (!_wcsicmp(OUTPUT_FORMAT, L"jpg") || !_wcsicmp(OUTPUT_FORMAT, L"jpeg"))
    {
        return F_JPEG;
    }
    if (!_wcsicmp(OUTPUT_FORMAT, L"bmp"))
    {
        return F_BMP;
    }
    if (!_wcsicmp(OUTPUT_FORMAT, L"tiff") || !_wcsicmp(OUTPUT_FORMAT, L"tif"))
    {
        return F_TIFF;
    }
    return F_UNKNOWN;
}

static const GUID *container_guid(Format format)
{
    switch (format)
    {
        case F_PNG:
            return &GUID_ContainerFormatPng;
        case F_JPEG:
            return &GUID_ContainerFormatJpeg;
        case F_TIFF:
            return &GUID_ContainerFormatTiff;
        case F_BMP:
            return &GUID_ContainerFormatBmp;
        default:
            return NULL;
    }
}

static int wic_decode(Page *p, IWICBitmapSource **source, UINT *frames)
{
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    HRESULT hr;
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr))
    {
        hr = IWICStream_InitializeFromMemory(stream, p->data, (DWORD)p->size);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                        WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapDecoder_GetFrameCount(decoder, frames);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameDecode_GetSize(frame, &p->w, &p->h);
    }
    if (SUCCEEDED(hr) && !dimensions(p))
    {
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
    {
        *source = (IWICBitmapSource *)frame;
        frame = NULL;
    }
    if (frame)
    {
        IWICBitmapFrameDecode_Release(frame);
    }
    if (decoder)
    {
        IWICBitmapDecoder_Release(decoder);
    }
    if (stream)
    {
        IWICStream_Release(stream);
    }
    return SUCCEEDED(hr)
               ? 1
               : fail("Windows WIC cannot decode this format (0x%08lx)", (unsigned long)hr);
}

/* Validate actual pixel decoding before preserving a standard image container.
   Windows built-in codecs cover these five formats. Other original containers
   (WebP/JP2/J2K/DjVu) remain extractable without an optional installed decoder. */
static int validate_standard(Page *p)
{
    if (p->format != F_PNG && p->format != F_JPEG && p->format != F_TIFF && p->format != F_BMP &&
        p->format != F_GIF)
    {
        return 1;
    }
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    uint8_t *buffer = NULL;
    HRESULT hr;
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr))
    {
        hr = IWICStream_InitializeFromMemory(stream, p->data, (DWORD)p->size);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                        WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapDecoder_GetFrameCount(decoder, &p->frames);
    }
    for (UINT i = 0; SUCCEEDED(hr) && i < p->frames; i++)
    {
        UINT width = 0, height = 0;
        hr = IWICBitmapDecoder_GetFrame(decoder, i, &frame);
        if (SUCCEEDED(hr))
        {
            hr = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
        }
        if (SUCCEEDED(hr) && (!width || !height || (uint64_t)width * height > MAX_IMAGE_PIXELS))
        {
            hr = E_INVALIDARG;
        }
        if (SUCCEEDED(hr) && i == 0)
        {
            p->w = width;
            p->h = height;
        }
        if (SUCCEEDED(hr))
        {
            hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
        }
        if (SUCCEEDED(hr))
        {
            hr = IWICFormatConverter_Initialize(
                converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
        }
        size_t stride = (size_t)width * 4, rows = height < 64 ? height : 64;
        if (SUCCEEDED(hr))
        {
            buffer = (uint8_t *)malloc(stride * rows);
            if (!buffer)
            {
                hr = E_OUTOFMEMORY;
            }
        }
        for (UINT y = 0; SUCCEEDED(hr) && y < height; y += 64)
        {
            WICRect rect = {0, (INT)y, (INT)width, (INT)((height - y) < 64 ? (height - y) : 64)};
            hr = IWICFormatConverter_CopyPixels(converter, &rect, (UINT)stride,
                                                (UINT)(stride * rect.Height), buffer);
        }
        free(buffer);
        buffer = NULL;
        if (converter)
        {
            IWICFormatConverter_Release(converter);
            converter = NULL;
        }
        if (frame)
        {
            IWICBitmapFrameDecode_Release(frame);
            frame = NULL;
        }
    }
    if (decoder)
    {
        IWICBitmapDecoder_Release(decoder);
    }
    if (stream)
    {
        IWICStream_Release(stream);
    }
    return SUCCEEDED(hr) ? 1 : fail("Invalid standard image (WIC 0x%08lx)", (unsigned long)hr);
}

static int encode_wic(Page *p, Format format, const wchar_t *temporary)
{
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    IWICPalette *palette = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapSource *source = NULL;
    IWICFormatConverter *converter = NULL;
    HRESULT hr = S_OK;
    UINT frames = 1;
    if (p->format == F_FAX)
    {
        if (!decode_fax(p))
        {
            return 0;
        }
        hr = IWICImagingFactory_CreateBitmapFromMemory(
            factory, p->w, p->h, &GUID_WICPixelFormat1bppIndexed, (UINT)p->stride,
            (UINT)(p->stride * p->h), p->pixels, &bitmap);
        if (SUCCEEDED(hr))
        {
            hr = IWICImagingFactory_CreatePalette(factory, &palette);
        }
        WICColor colors[2] = {0xff000000u, 0xffffffffu};
        if (SUCCEEDED(hr))
        {
            hr = IWICPalette_InitializeCustom(palette, colors, 2);
        }
        if (SUCCEEDED(hr))
        {
            hr = IWICBitmap_SetPalette(bitmap, palette);
        }
        source = (IWICBitmapSource *)bitmap;
        bitmap = NULL;
    }
    else
    {
        if (p->format == F_DJVU)
        {
            return fail("DjVu supports original extraction in auto mode only");
        }
        if (!wic_decode(p, &source, &frames))
        {
            return 0;
        }
        if (frames > 1)
        {
            IWICBitmapSource_Release(source);
            return fail("Use auto mode to preserve multi-frame images");
        }
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateStream(factory, &stream);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICStream_InitializeFromFilename(stream, temporary, GENERIC_WRITE);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateEncoder(factory, container_guid(format), NULL, &encoder);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    }
    if (SUCCEEDED(hr) && format == F_JPEG && properties)
    {
        PROPBAG2 option = {0};
        VARIANT value;
        VariantInit(&value);
        option.pstrName = L"ImageQuality";
        value.vt = VT_R4;
        value.fltVal = JPEG_QUALITY;
        IPropertyBag2_Write(properties, 1, &option, &value);
    }
    if (SUCCEEDED(hr) && format == F_TIFF && p->format == F_FAX && properties)
    {
        PROPBAG2 option = {0};
        VARIANT value;
        VariantInit(&value);
        option.pstrName = L"TiffCompressionMethod";
        value.vt = VT_UI1;
        value.bVal = WICTiffCompressionCCITT4;
        IPropertyBag2_Write(properties, 1, &option, &value);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_SetSize(frame, p->w, p->h);
    }
    if (SUCCEEDED(hr) && IMAGE_DPI > 0)
    {
        hr = IWICBitmapFrameEncode_SetResolution(frame, IMAGE_DPI, IMAGE_DPI);
    }
    WICPixelFormatGUID pixel = (p->format == F_FAX && format != F_JPEG)
                                   ? GUID_WICPixelFormat1bppIndexed
                                   : GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel);
    }
    if (SUCCEEDED(hr) && palette && IsEqualGUID(&pixel, &GUID_WICPixelFormat1bppIndexed))
    {
        hr = IWICBitmapFrameEncode_SetPalette(frame, palette);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICFormatConverter_Initialize(converter, source, &pixel, WICBitmapDitherTypeNone,
                                            palette, 0, WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)converter, NULL);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_Commit(frame);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_Commit(encoder);
    }
    if (converter)
    {
        IWICFormatConverter_Release(converter);
    }
    if (source)
    {
        IWICBitmapSource_Release(source);
    }
    if (bitmap)
    {
        IWICBitmap_Release(bitmap);
    }
    if (palette)
    {
        IWICPalette_Release(palette);
    }
    if (properties)
    {
        IPropertyBag2_Release(properties);
    }
    if (frame)
    {
        IWICBitmapFrameEncode_Release(frame);
    }
    if (encoder)
    {
        IWICBitmapEncoder_Release(encoder);
    }
    if (stream)
    {
        IWICStream_Release(stream);
    }
    return SUCCEEDED(hr) ? 1 : fail("Image encoding failed (WIC 0x%08lx)", (unsigned long)hr);
}

static int write_raw(const wchar_t *path, const uint8_t *data, size_t size)
{
    HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;
    if (file == INVALID_HANDLE_VALUE)
    {
        return fail("Cannot create temporary output (Windows error %lu)", GetLastError());
    }
    int result = WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
    CloseHandle(file);
    return result ? 1 : fail("Output write failed");
}

static wchar_t *target_path(const wchar_t *output, const wchar_t *relative, Format format)
{
    wchar_t *path = join(output, relative);
    if (!path)
    {
        return NULL;
    }
    wchar_t *dot = wcsrchr(path, L'.');
    if (dot)
    {
        *dot = 0;
    }
    size_t n = wcslen(path) + wcslen(extension(format)) + 1;
    wchar_t *result = (wchar_t *)realloc(path, n * sizeof(wchar_t));
    if (!result)
    {
        free(path);
        return NULL;
    }
    wcscat(result, extension(format));
    return result;
}

/* 批处理：单文件转换、原子提交与目录枚举 */
static int convert(const wchar_t *input, const wchar_t *relative, const wchar_t *output,
                   wchar_t **target, Page *p, Format *format)
{
    if (!read_file(input, p) || !prepare(p))
    {
        return 0;
    }
    *format = output_format(p);
    if (*format == F_UNKNOWN)
    {
        return fail("Unsupported output format; choose auto/png/tiff/bmp/jpg");
    }
    *target = target_path(output, relative, *format);
    if (!*target)
    {
        return fail("Output path too long or out of memory");
    }
    if (exists(*target) && !OVERWRITE_EXISTING)
    {
        return 2;
    }
    if (!parent_dirs(*target))
    {
        return 0;
    }
    size_t n = wcslen(*target) + 80;
    wchar_t *temporary = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!temporary)
    {
        return fail("Out of memory");
    }
    _snwprintf(temporary, n, L"%ls.tmp.%lu.%llu", *target, GetCurrentProcessId(),
               (unsigned long long)GetTickCount64());
    int result;
    if (p->format != F_FAX && *format == p->format)
    {
        result = validate_standard(p) && write_raw(temporary, p->data, p->size);
    }
    else
    {
        result = encode_wic(p, *format, temporary);
    }
    if (result &&
        !MoveFileExW(temporary, *target, OVERWRITE_EXISTING ? MOVEFILE_REPLACE_EXISTING : 0))
    {
        DWORD code = GetLastError();
        if (!OVERWRITE_EXISTING && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS))
        {
            result = 2;
        }
        else
        {
            result = fail("Cannot commit output (Windows error %lu)", code);
        }
    }
    DeleteFileW(temporary);
    free(temporary);
    return result;
}

static int push_entry(Entries *list, const wchar_t *path, const wchar_t *relative)
{
    if (list->size == list->cap)
    {
        size_t cap = list->cap ? list->cap * 2 : 256;
        Entry *p = (Entry *)realloc(list->items, cap * sizeof(Entry));
        if (!p)
        {
            return fail("Out of memory collecting paths");
        }
        list->items = p;
        list->cap = cap;
    }
    Entry e = {duplicate(path), duplicate(relative)};
    if (!e.path || !e.relative)
    {
        free(e.path);
        free(e.relative);
        return fail("Out of memory");
    }
    list->items[list->size++] = e;
    return 1;
}

static int collect(Entries *list, const wchar_t *dir, const wchar_t *relative,
                   const wchar_t *output, int exclude_output, unsigned depth)
{
    if (depth > 128)
    {
        return fail("Input directory nesting too deep");
    }
    wchar_t *pattern = join(dir, L"*");
    if (!pattern)
    {
        return fail("Input path too long");
    }
    WIN32_FIND_DATAW info;
    HANDLE search = FindFirstFileW(pattern, &info);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE)
    {
        DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND
                   ? 1
                   : fail("Cannot enumerate input directory (Windows error %lu)", code);
    }
    int result = 1;
    do
    {
        if (!wcscmp(info.cFileName, L".") || !wcscmp(info.cFileName, L".."))
        {
            continue;
        }
        wchar_t *path = join(dir, info.cFileName),
                *rel = relative[0] ? join(relative, info.cFileName) : duplicate(info.cFileName);
        if (!path || !rel)
        {
            free(path);
            free(rel);
            result = fail("Path too long");
            break;
        }
        if (exclude_output && path_inside(path, output))
        {
            free(path);
            free(rel);
            continue;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (RECURSIVE && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                result = collect(list, path, rel, output, exclude_output, depth + 1);
            }
        }
        else
        {
            const wchar_t *ext = wcsrchr(info.cFileName, L'.');
            if (ext && !_wcsicmp(ext, L".pdg"))
            {
                result = push_entry(list, path, rel);
            }
        }
        free(path);
        free(rel);
        if (!result)
        {
            break;
        }
    } while (FindNextFileW(search, &info));
    FindClose(search);
    return result;
}

static int sort_entries(const void *a, const void *b)
{
    return _wcsicmp(((const Entry *)a)->relative, ((const Entry *)b)->relative);
}

/* 统计报告：CSV 与 JSON 编码 */
static void quoted(FILE *file, const char *s, int json)
{
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (*p == '"')
        {
            fputs(json ? "\\\"" : "\"\"", file);
        }
        else if (json && *p == '\\')
        {
            fputs("\\\\", file);
        }
        else if (json && *p < 32)
        {
            fprintf(file, "\\u%04x", *p);
        }
        else
        {
            fputc(*p, file);
        }
    }
    fputc('"', file);
}

static void report_row(const Entry *entry, const wchar_t *target, int result, Page *p,
                       Format format, double seconds)
{
    char *input = utf8(entry->path), *out = utf8(target ? target : L"");
    const char *status = result == 1 ? "success" : result == 2 ? "skipped" : "failed";
    if (csv_report)
    {
        quoted(csv_report, input, 0);
        fputc(',', csv_report);
        quoted(csv_report, out, 0);
        fprintf(csv_report, ",%s,%s,%u,%u,%u,%u,0x%02x,%.6f,", status, format_name(format), p->w,
                p->h, p->frames, p->version, p->kind, seconds);
        quoted(csv_report, result ? "" : error_text, 0);
        fputc('\n', csv_report);
    }
    if (json_report)
    {
        if (!report_first)
        {
            fputs(",\n", json_report);
        }
        report_first = 0;
        fputs("    {\"input\":", json_report);
        quoted(json_report, input, 1);
        fputs(",\"output\":", json_report);
        quoted(json_report, out, 1);
        fprintf(json_report,
                ",\"status\":\"%s\",\"format\":\"%s\",\"width\":%u,\"height\":%u,\"version\":%u,"
                "\"encoding\":%u,\"seconds\":%.6f,\"error\":",
                status, format_name(format), p->w, p->h, p->version, p->kind, seconds);
        quoted(json_report, result ? "" : error_text, 1);
        fputc('}', json_report);
    }
    free(input);
    free(out);
}

static int normalize(const wchar_t *source, const wchar_t *base, wchar_t dest[PATH_CAP])
{
    wchar_t *p = source[0] && ((source[1] == L':') || (source[0] == L'\\' && source[1] == L'\\'))
                     ? duplicate(source)
                     : join(base, source);
    if (!p)
    {
        return 0;
    }
    DWORD n = GetFullPathNameW(p, PATH_CAP, dest, NULL);
    free(p);
    if (!n || n >= PATH_CAP)
    {
        return 0;
    }
    size_t len = wcslen(dest);
    while (len > 3 && dest[len - 1] == L'\\')
    {
        dest[--len] = 0;
    }
    return 1;
}

/* 程序入口：解析参数、逐文件处理与资源清理 */
int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    wchar_t *input = (wchar_t *)calloc(PATH_CAP, sizeof(wchar_t));
    wchar_t *output = (wchar_t *)calloc(PATH_CAP, sizeof(wchar_t));
    wchar_t *base = (wchar_t *)calloc(PATH_CAP, sizeof(wchar_t));
    Entries list = {0};
    int exit_code = 1;
    if (!input || !output || !base)
    {
        fprintf(stderr, "Out of memory\n");
        goto cleanup;
    }
    GetModuleFileNameW(NULL, base, PATH_CAP);
    wchar_t *slash = wcsrchr(base, L'\\');
    if (slash)
    {
        *slash = 0;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--help"))
    {
        printf("PDG Converter (C)\nUsage: pdg_converter.exe [input_dir output_dir] [--format "
               "auto|png|tiff|bmp|jpg] [--overwrite] [--no-recursive] [--summary|--no-summary] "
               "[--reports|--no-reports]\nPer-file progress is always displayed. No arguments: use "
               "source settings.\n");
        exit_code = 0;
        goto cleanup;
    }
    const wchar_t *input_setting = INPUT_DIR, *output_setting = OUTPUT_DIR;
    int opt = 1;
    if (argc > 1)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Provide both input and output directories\n");
            goto cleanup;
        }
        input_setting = argv[1];
        output_setting = argv[2];
        opt = 3;
    }
    for (; opt < argc; opt++)
    {
        if (!wcscmp(argv[opt], L"--overwrite"))
        {
            OVERWRITE_EXISTING = 1;
        }
        else if (!wcscmp(argv[opt], L"--no-recursive"))
        {
            RECURSIVE = 0;
        }
        else if (!wcscmp(argv[opt], L"--no-summary") || !wcscmp(argv[opt], L"--quiet"))
        {
            SHOW_SUMMARY = 0;
        }
        else if (!wcscmp(argv[opt], L"--no-reports"))
        {
            WRITE_REPORTS = 0;
        }
        else if (!wcscmp(argv[opt], L"--summary") || !wcscmp(argv[opt], L"--verbose"))
        {
            SHOW_SUMMARY = 1;
        }
        else if (!wcscmp(argv[opt], L"--reports"))
        {
            WRITE_REPORTS = 1;
        }
        else if (!wcscmp(argv[opt], L"--format") && opt + 1 < argc)
        {
            OUTPUT_FORMAT = argv[++opt];
        }
        else
        {
            fprintf(stderr, "Unknown argument; use --help\n");
            goto cleanup;
        }
    }
    if (_wcsicmp(OUTPUT_FORMAT, L"auto") && _wcsicmp(OUTPUT_FORMAT, L"png") &&
        _wcsicmp(OUTPUT_FORMAT, L"tiff") && _wcsicmp(OUTPUT_FORMAT, L"tif") &&
        _wcsicmp(OUTPUT_FORMAT, L"bmp") && _wcsicmp(OUTPUT_FORMAT, L"jpg") &&
        _wcsicmp(OUTPUT_FORMAT, L"jpeg"))
    {
        fprintf(stderr, "Unsupported output format\n");
        goto cleanup;
    }
    if (!normalize(input_setting, base, input) || !normalize(output_setting, base, output))
    {
        fprintf(stderr, "Invalid path\n");
        goto cleanup;
    }
    if (!_wcsicmp(input, output))
    {
        fprintf(stderr, "Input and output directories must differ\n");
        goto cleanup;
    }
    DWORD attr = GetFileAttributesW(input);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        fprintf(stderr, "Input directory does not exist\n");
        goto cleanup;
    }
    if (!collect(&list, input, L"", output, path_inside(output, input), 0) || !mkdirs(output))
    {
        fprintf(stderr, "%s\n", error_text);
        goto cleanup;
    }
    qsort(list.items, list.size, sizeof(Entry), sort_entries);
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int com_ok = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory, (void **)&factory);
    }
    if (FAILED(hr))
    {
        fprintf(stderr, "Cannot initialize Windows WIC: 0x%08lx\n", (unsigned long)hr);
        if (com_ok)
        {
            CoUninitialize();
        }
        goto cleanup;
    }
    wchar_t *csvname = NULL, *jsonname = NULL;
    if (WRITE_REPORTS)
    {
        SYSTEMTIME time;
        FILETIME clock_time;
        GetSystemTimeAsFileTime(&clock_time);
        ULARGE_INTEGER clock_value;
        clock_value.LowPart = clock_time.dwLowDateTime;
        clock_value.HighPart = clock_time.dwHighDateTime;
        clock_value.QuadPart += 8ULL * 3600 * 10000000;
        clock_time.dwLowDateTime = clock_value.LowPart;
        clock_time.dwHighDateTime = clock_value.HighPart;
        FileTimeToSystemTime(&clock_time, &time);
        wchar_t stamp[160], name[168];
        _snwprintf(stamp, 160, L"conversion_report_%04u%02u%02u_%02u%02u%02u_%lu", time.wYear,
                   time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                   GetCurrentProcessId());
        _snwprintf(name, 168, L"%ls.csv", stamp);
        csvname = join(output, name);
        _snwprintf(name, 168, L"%ls.json", stamp);
        jsonname = join(output, name);
        if (!csvname || !jsonname)
        {
            fprintf(stderr, "Out of memory\n");
            goto finish;
        }
        csv_report = _wfopen(csvname, L"wb");
        json_report = _wfopen(jsonname, L"wb");
        if (!csv_report || !json_report)
        {
            fprintf(stderr, "Cannot create conversion reports\n");
            goto finish;
        }
        fputs("\xef\xbb\xbfinput,output,status,format,width,height,frames,version,encoding,seconds,"
              "error\n",
              csv_report);
        fputs("{\n  \"files\":[\n", json_report);
    }
    printf("PDG Converter (C): %zu files\n", list.size);
    double started = now_seconds();
    for (size_t i = 0; i < list.size; i++)
    {
        Page page = {0};
        page.frames = 1;
        wchar_t *target = NULL;
        Format format = F_UNKNOWN;
        error_text[0] = 0;
        double start = now_seconds();
        int result =
            convert(list.items[i].path, list.items[i].relative, output, &target, &page, &format);
        double elapsed = now_seconds() - start;
        if (result == 1)
        {
            successes++;
        }
        else if (result == 2)
        {
            skipped++;
        }
        else
        {
            failures++;
        }
        if (WRITE_REPORTS)
        {
            report_row(&list.items[i], target, result, &page, format, elapsed);
        }
        char *name = utf8(list.items[i].relative);
        FILE *progress_stream = result ? stdout : stderr;
        fprintf(progress_stream, "[%zu/%zu] %s %s%s%s\n", i + 1, list.size,
                result == 1   ? "OK"
                : result == 2 ? "SKIP"
                              : "FAIL",
                name, result ? "" : " : ", result ? "" : error_text);
        fflush(progress_stream);
        free(name);
        free(target);
        free_page(&page);
    }
    double elapsed = now_seconds() - started;
    if (json_report)
    {
        fprintf(json_report,
                "\n  ],\n  "
                "\"summary\":{\"total\":%zu,\"success\":%zu,\"skipped\":%zu,\"failed\":%zu,"
                "\"elapsed_seconds\":%.6f},\n  \"report_timezone\":\"UTC+08:00\"\n}\n",
                list.size, successes, skipped, failures, elapsed);
    }
    if (SHOW_SUMMARY)
    {
        printf("Finished: %zu success, %zu skipped, %zu failed; %.3f seconds\n", successes, skipped,
               failures, elapsed);
        if (jsonname)
        {
            char *reportpath = utf8(jsonname);
            printf("Report: %s\n", reportpath);
            free(reportpath);
        }
    }
    exit_code = failures ? 2 : 0;
finish:
    if (csv_report)
    {
        fclose(csv_report);
    }
    if (json_report)
    {
        fclose(json_report);
    }
    free(csvname);
    free(jsonname);
    if (factory)
    {
        IWICImagingFactory_Release(factory);
    }
    if (com_ok)
    {
        CoUninitialize();
    }
cleanup:
    for (size_t i = 0; i < list.size; i++)
    {
        free(list.items[i].path);
        free(list.items[i].relative);
    }
    free(list.items);
    free(input);
    free(output);
    free(base);
    return exit_code;
}
