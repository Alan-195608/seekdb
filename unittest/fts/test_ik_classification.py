#!/usr/bin/env python3
"""Compile real IK classification/Unicode/decoder sources without the server runtime.

Only runtime headers (logging, status codes, charset lookup) are replaced by a
small adapter. Decoder and classification implementations are taken verbatim
from the checkout, and compared with the frozen pre-optimization revision.
This is a focused differential test/microbenchmark, not the official FTS score.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CHAR_UTIL = 'src/storage/fts/ik/ob_ik_char_util.h'
BASE = 'fc9629d5706328c452005cf0f3f89f4662bf2db2'


def no_includes(source):
    return re.sub(r'^#include[^\n]*', '', source, flags=re.M)


def source():
    prefix = r'''
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <array>
using ob_wc_t = unsigned int;
using uint16 = uint16_t;
using uint32 = uint32_t;
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -4002
#define OB_UNEXPECT_INTERNAL_ERROR -4100
#define OB_NOT_SUPPORTED -4007
#define OB_CS_ILSEQ 0
#define OB_CS_ILUNI 0
#define OB_CS_TOOSMALL -101
#define OB_CS_TOOSMALL2 -102
#define OB_CS_TOOSMALL3 -103
#define OB_CS_TOOSMALL4 -104
#define OB_CS_TOOSMALLN(n) (-100-(n))
#define OB_FAIL(x) ((ret = (x)) != OB_SUCCESS)
#define OB_ISNULL(x) ((x) == nullptr)
#define OB_LIKELY(x) (x)
#define OB_UNLIKELY(x) (x)
#define STORAGE_FTS_LOG(...)
#define UNUSED(x) (void)(x)
#define ARRAYSIZEOF(x) (sizeof(x)/sizeof((x)[0]))
namespace oceanbase { namespace common {
enum ObCharsetType { CHARSET_INVALID=0, CHARSET_BINARY=1, CHARSET_UTF8MB4=2,
                     CHARSET_GBK=3, CHARSET_UTF16=4, CHARSET_UTF16LE=10 };
using ObCollationType = ObCharsetType;
struct ObCharset {
  static ObCharsetType charset_type_by_coll(ObCollationType c) { return c; }
};
}}
using namespace oceanbase::common;
'''
    # Compile the original decoder implementation, not a Python reimplementation.
    mb = (ROOT / 'src/oblib/lib/charset/mb_wc.h').read_text()
    start = mb.index('template <bool RANGE_CHECK, bool SUPPORT_MB4>\nstatic ALWAYS_INLINE int')
    end = mb.index('\nextern "C"', start)
    prefix += '#define ALWAYS_INLINE __attribute__((always_inline)) inline\n' + mb[start:end]
    helper = (ROOT / 'src/oblib/lib/charset/ob_charset_string_helper.h').read_text()
    helper = helper[helper.index('namespace oceanbase'):helper.index('class ObFastStringScanner')]
    prefix += helper + '\n}}\n'
    prefix += no_includes((ROOT / 'src/storage/fts/utils/unicode_utils.h').read_text())
    prefix += no_includes((ROOT / 'src/storage/fts/utils/unicode_utils.cpp').read_text())
    baseline = subprocess.check_output(['git', 'show', f'{BASE}:{CHAR_UTIL}'], cwd=ROOT, text=True)
    baseline = baseline.replace('ObFTCharUtil', 'BaselineCharUtil')
    baseline = baseline.replace('_OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_', 'BASELINE_CHAR_UTIL_H_')
    prefix += no_includes(baseline)
    prefix += no_includes((ROOT / CHAR_UTIL).read_text())
    return prefix


MAIN = r'''
using namespace oceanbase::storage;
static uint64_t checks = 0;
static void check(const char *p, uint8_t n, ObCollationType cs) {
  BaselineCharUtil::CharType a;
  ObFTCharUtil::CharType b;
  int ra = BaselineCharUtil::classify_first_char(cs, p, n, a);
  int rb = ObFTCharUtil::classify_first_char(cs, p, n, b);
  if (ra != rb || (ra == OB_SUCCESS && int(a) != int(b))) {
    std::fprintf(stderr, "Mismatch charset=%d length=%u ret=%d/%d type=%d/%d\n",
                 int(cs), n, ra, rb, int(a), int(b));
    std::exit(1);
  }
  ++checks;
}
static void expect(const char *p, uint8_t n, ObFTCharUtil::CharType want) {
  ObFTCharUtil::CharType got;
  if (ObFTCharUtil::classify_first_char(CHARSET_UTF8MB4, p, n, got) != OB_SUCCESS || got != want)
    std::abort();
}
using Classify = int (*)(ObCollationType, const char *, uint8_t, int &);
__attribute__((noinline)) static int old_classify(ObCollationType c, const char *p, uint8_t n, int &out) {
  BaselineCharUtil::CharType t;
  int r = BaselineCharUtil::classify_first_char(c,p,n,t); out=int(t); return r;
}
__attribute__((noinline)) static int new_classify(ObCollationType c, const char *p, uint8_t n, int &out) {
  ObFTCharUtil::CharType t;
  int r = ObFTCharUtil::classify_first_char(c,p,n,t); out=int(t); return r;
}
static double bench(Classify f, const std::vector<std::array<char,4>> &chars,
                    const std::vector<uint8_t> &lens) {
  volatile uint64_t checksum = 0;
  auto begin = std::chrono::steady_clock::now();
  for (int pass=0; pass<500; ++pass) {
    for (size_t i=0; i<chars.size(); ++i) {
      int t; int r=f(CHARSET_UTF8MB4,chars[i].data(),lens[i],t);
      checksum += uint64_t(t) + uint64_t(r == OB_SUCCESS);
    }
  }
  if (checksum == 0) std::abort();
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
}
int main(int argc, char **) {
  using T=ObFTCharUtil::CharType;
  expect("A",1,T::ENGLISH_LETTER); expect("9",1,T::ARABIC_LETTER);
  expect("中",3,T::CHINESE); expect("あ",3,T::OTHER_CJK);
  expect("Ａ",3,T::ENGLISH_LETTER); expect("９",3,T::ARABIC_LETTER);
  expect("😀",4,T::USELESS); expect("_",1,T::USELESS);
  std::vector<std::array<char,4>> chars;
  std::vector<uint8_t> lens;
  for (uint32_t cp=0; cp<=0x10ffff; ++cp) {
    if (cp>=0xd800 && cp<=0xdfff) continue;
    unsigned char bytes[4] = {};
    int n=ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp,bytes,bytes+4);
    check(reinterpret_cast<const char *>(bytes),n,CHARSET_UTF8MB4);
    for (int cut=0; cut<n; ++cut) check(reinterpret_cast<const char *>(bytes),cut,CHARSET_UTF8MB4);
    // Exercise the existing UTF16/LE dispatch and generic decoder semantics.
    check(reinterpret_cast<const char *>(bytes),n,CHARSET_UTF16);
    check(reinterpret_cast<const char *>(bytes),n,CHARSET_UTF16LE);
    if ((cp>=0x4e00 && cp<0x6000) || (cp>=32 && cp<=126)) {
      std::array<char,4> b={}; std::memcpy(b.data(),bytes,n);
      chars.push_back(b); lens.push_back(n);
    }
  }
  for (unsigned x=0; x<65536; ++x) {
    char bytes[4]={char(x>>8),char(x),char(0x80),char(0xff)};
    for (int n=0;n<=4;++n) check(bytes,n,CHARSET_UTF8MB4);
  }
  for (auto c : {CHARSET_UTF8MB4,CHARSET_UTF16,CHARSET_UTF16LE,CHARSET_GBK})
    check(nullptr,1,c);
  std::printf("PASS: %llu differential cases; explicit category assertions passed\n",
              static_cast<unsigned long long>(checks));
  if (argc>1) {
    // Alternate order to reduce warm-up/order bias; report every sample.
    for (int sample=0;sample<5;++sample) {
      double old_ms, new_ms;
      if (sample%2) { new_ms=bench(new_classify,chars,lens); old_ms=bench(old_classify,chars,lens); }
      else { old_ms=bench(old_classify,chars,lens); new_ms=bench(new_classify,chars,lens); }
      std::printf("sample=%d baseline_ms=%.3f optimized_ms=%.3f reduction_pct=%.2f\n",
                  sample,old_ms,new_ms,100*(old_ms-new_ms)/old_ms);
    }
  }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ik-classification-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        binary = Path(tmp) / 'test'
        cpp.write_text(source() + MAIN)
        cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra',
               '-Wno-unused-parameter', str(cpp), '-o', str(binary)]
        if args.sanitize:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)] + (['benchmark'] if args.benchmark else []), check=True)


if __name__ == '__main__':
    main()
