import re

with open('main.cpp', 'r') as f:
    content = f.read()

avx2_match = re.search(r'(static ScanResult ScanBandAVX2.*?return result;\n})', content, re.DOTALL)
scalar_match = re.search(r'(static ScanResult ScanBandScalar.*?return result;\n})', content, re.DOTALL)
coarse_match = re.search(r'(static ScanResult CoarseScan.*?return result;\n})', content, re.DOTALL)
full_match = re.search(r'(static ScanResult ScanRegionFull.*?return ScanBandScalar[^\}]+})', content, re.DOTALL)
mt_match = re.search(r'(static ScanResult ScanRegionMT.*?return merged;\n})', content, re.DOTALL)

with open('src/core/Scanner.h', 'w') as f:
    f.write('''#pragma once
#include <cstdint>
#include <vector>
#include "ScannerTypes.h"
#include "../utils/ThreadPool.h"

class Scanner {
public:
    static void InitFeatureSupport();
    static bool HasAVX2();

    static ScanResult ScanBandScalar(
        const uint8_t* pixels, int pitch,
        int regionX, int regionY, int regionW, int regionH,
        int bandY, int bandH,
        int targetY, int targetCb, int targetCr,
        int threshold);

    static ScanResult ScanBandAVX2(
        const uint8_t* pixels, int pitch,
        int regionX, int regionY, int regionW, int regionH,
        int bandY, int bandH,
        int targetY, int targetCb, int targetCr,
        int threshold);

    static ScanResult CoarseScan(
        const uint8_t* pixels, int pitch,
        int regionX, int regionY, int regionW, int regionH,
        const YCbCrTarget& tgt, int threshold, int stride = 2);

    static ScanResult ScanRegionFull(
        const uint8_t* pixels, int pitch,
        int regionX, int regionY, int regionW, int regionH,
        const YCbCrTarget& tgt, int threshold);

    static ScanResult ScanRegionMT(
        const uint8_t* pixels, int pitch,
        int regionX, int regionY, int regionW, int regionH,
        const YCbCrTarget& tgt, int threshold,
        std::vector<TileResult>& tileResults, int tileSize = 8);
};
''')

with open('src/core/Scanner.cpp', 'w') as f:
    f.write('#include "Scanner.h"\n#include <algorithm>\n#include <cmath>\n#include <immintrin.h>\n\n')
    f.write('static bool g_hasAVX2 = false;\n\n')
    f.write('void Scanner::InitFeatureSupport() {\n    __builtin_cpu_init();\n    g_hasAVX2 = __builtin_cpu_supports("avx2");\n}\n\n')
    f.write('bool Scanner::HasAVX2() { return g_hasAVX2; }\n\n')

    f.write(avx2_match.group(1).replace('static ScanResult', 'ScanResult Scanner::') + '\n\n')
    f.write(scalar_match.group(1).replace('static ScanResult', 'ScanResult Scanner::') + '\n\n')
    c = coarse_match.group(1).replace('static ScanResult', 'ScanResult Scanner::').replace('int threshold)', 'int threshold, int stride)').replace('COARSE_STRIDE', 'stride')
    f.write(c + '\n\n')
    f.write(full_match.group(1).replace('static ScanResult', 'ScanResult Scanner::') + '\n\n')
    m = mt_match.group(1).replace('static ScanResult', 'ScanResult Scanner::').replace('int threshold,\n    std::vector<TileResult>& tileResults)', 'int threshold, std::vector<TileResult>& tileResults, int tileSize)').replace('TILE_SIZE', 'tileSize')
    f.write(m + '\n\n')
