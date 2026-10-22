#!/usr/bin/env python3
"""Down-level LLVM 21 textual IR to what llvm-as 18 accepts (only the
syntax the metal2vulkan AIR fixtures use). Used by the host test when no
LLVM >= 21 is installed; on a Mac use the real llvm-as instead."""
import re, sys
s = open(sys.argv[1]).read()
s = s.replace("captures(none)", "nocapture")
s = re.sub(r"\bcaptures\([^)]*\)", "", s)
s = re.sub(r"\bnuw nusw\b|\bnusw nuw\b|\bnusw\b", "", s)          # gep flags (LLVM 19+)
s = re.sub(r"getelementptr inbounds nuw", "getelementptr inbounds", s)
s = re.sub(r"\bsamesign\b", "", s)                                  # icmp samesign (LLVM 20)
s = re.sub(r"\binitializes\(\([^)]*\)\)", "", s)                    # initializes((0, 4)) (LLVM 19)
s = re.sub(r"\bnoalias\.addrspace\b[^,\n]*", "", s)
open(sys.argv[2], "w").write(s)
