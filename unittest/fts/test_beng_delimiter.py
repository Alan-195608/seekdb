#!/usr/bin/env python3
"""Exhaustively compare the actual delimiter implementation with charset macros."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / 'src/share/text_analysis/ob_token_stream.cpp').read_text()
    method = re.search(r'bool ObTextWhitespaceTokenizer::found_delimiter\(\)\n\{.*?\n\}', source, re.S).group()
    header = (ROOT / 'src/oblib/lib/charset/ob_ctype.h').read_text()
    macros = '\n'.join(line for line in header.splitlines() if re.match(
        r'#define\s+(?:_MY_(?:SPC|PNT|CTR)|ob_(?:isspace|iscntrl|ispunct))\b', line))
    program = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct Charset { const unsigned char *ctype; };
struct Datum { const char *ptr_; };
class ObTextWhitespaceTokenizer {
public:
  Charset *cs_;
  Datum *input_doc_;
  unsigned trav_pos_;
  bool found_delimiter();
};
''' + macros + '\n' + method + r'''
int main() {
  unsigned char table[257] = {};
  char input[256];
  for (unsigned i=0; i<256; ++i) input[i]=static_cast<char>(i);
  Datum doc{input}; Charset cs{table};
  ObTextWhitespaceTokenizer tokenizer{&cs, &doc, 0};
  unsigned checks=0;
  for (unsigned flags=0; flags<256; ++flags) {
    for (unsigned i=1; i<=256; ++i) table[i]=static_cast<unsigned char>(flags);
    for (unsigned byte=0; byte<256; ++byte) {
      tokenizer.trav_pos_=byte;
      bool expected=ob_isspace(&cs,input[byte]) || ob_iscntrl(&cs,input[byte]) || ob_ispunct(&cs,input[byte]);
      if (tokenizer.found_delimiter()!=expected) std::abort();
      ++checks;
    }
  }
  cs.ctype=nullptr;
  for (unsigned byte=0; byte<256; ++byte) {
    tokenizer.trav_pos_=byte;
    if (tokenizer.found_delimiter()) std::abort();
    ++checks;
  }
  std::printf("PASS: %u delimiter cases including null classification table\n",checks);
}
'''
    with tempfile.TemporaryDirectory(prefix='beng-delimiter-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(program)
        subprocess.run(['c++', '-std=c++17', '-O2', '-fsanitize=address,undefined',
                        str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
