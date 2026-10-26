# How rewritten code finds the host thread's slot block.
#
# Darwin (macOS / iOS): TPIDRRO_EL0 points at the thread's TSD array, so pthread key k lives at TP + 8k and the
# emitted code is a single "mrs xN, tpidrro_el0" before the key-page offset is added.
#
# Linux / Android: TPIDRRO_EL0 reads as 0 for native tasks, and the C library's thread pointer TPIDR_EL0 points at
# its TCB, where nothing sits at an app-chosen offset. Bionic reserves one TCB word for apps from API level 29,
# TLS_SLOT_APP (slot 2, TP + 16; bionic/libc/platform/bionic/tls_defines.h). The runtime stores the address of the
# thread's own slot block there (runtime/host_tp.c), so the emitted code is
#     mrs xN, tpidr_el0
#     ldr xN, [xN, #16]
# and the key page then holds byte offsets into that block. glibc (desktop Linux, used for tests under qemu) puts
# the executable's first TLS word at TP + 16; host_tp.c places a dedicated initialized __thread word there and
# checks the address at startup on every thread.
#
# The guest never sees either register: every guest mrs/msr of tpidr_el0 / tpidrro_el0 is rewritten to a slot
# access, so only code the rewriter emits ever reads the register named here. The default stays Darwin, so the
# verified hashes in assets/expected-v*.json are unchanged. DM_HOST_TP=tpidr_el0 selects Linux / Android.
import os

_MRS = {'tpidrro_el0': 0xD53BD060, 'tpidr_el0': 0xD53BD040}
HOST_TP = os.environ.get('DM_HOST_TP', 'tpidrro_el0')
if HOST_TP not in _MRS:
    raise SystemExit(f'DM_HOST_TP must be one of {sorted(_MRS)}, not {HOST_TP!r}')
HOST_TP_MRS = _MRS[HOST_TP]          # mrs xN, <HOST_TP> is HOST_TP_MRS | N
LINUX = HOST_TP == 'tpidr_el0'
SLOT_PTR_OFF = 16                    # bionic TLS_SLOT_APP * 8; glibc: the executable's first TLS word
LDR_IMM = 0xF9400000                 # ldr xt, [xn, #imm12*8]


def host_tp_words(reg):
    """Words that leave the thread's slot-block base in x<reg> (the key-page offset is added after them)."""
    assert 0 <= reg < 31
    words = [HOST_TP_MRS | reg]
    if LINUX:
        words.append(LDR_IMM | ((SLOT_PTR_OFF // 8) << 10) | (reg << 5) | reg)
    return words


HOST_TP_LEN = len(host_tp_words(0))  # 1 on Darwin, 2 on Linux / Android


def names_host_tp(op_str):
    """True when an mrs operand string ('x30, tpidrro_el0') reads the host thread register."""
    parts = op_str.replace(',', ' ').split()
    return len(parts) == 2 and parts[1].lower() == HOST_TP


def host_tp_at(q, i):
    """q: capstone instructions. If q[i:] starts with the host-thread-pointer sequence, return (its register name,
    index after it); otherwise None."""
    if i >= len(q) or q[i].mnemonic != 'mrs' or not names_host_tp(q[i].op_str):
        return None
    reg = q[i].op_str.split(',')[0].strip()
    if not LINUX:
        return reg, i + 1
    if i + 1 >= len(q):
        return None
    ld = q[i + 1]
    if ld.mnemonic != 'ldr' or ld.op_str.replace(' ', '') != f'{reg},[{reg},#0x{SLOT_PTR_OFF:x}]':
        return None
    return reg, i + 2


def host_tp_words_at(word_at, o, reg=None):
    """word_at(offset) -> 32-bit word. True when the host-thread-pointer sequence (for x<reg>, or any register)
    starts at offset o."""
    w = word_at(o)
    if w & 0xFFFFFFE0 != HOST_TP_MRS:
        return False
    r = w & 31
    if reg is not None and r != reg:
        return False
    return all(word_at(o + 4 * k) == x for k, x in enumerate(host_tp_words(r)))
