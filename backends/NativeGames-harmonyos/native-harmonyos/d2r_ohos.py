# Run a d2r tool (third_party/d2r: patch_nso.py, structck.py, equiv.py) for the HarmonyOS host register layout.
#
# Rewritten code finds a host thread slot as <slot base> + <key-page offset> (hostreg.py). The reference has two forms:
#   Darwin          slot base = TPIDRRO_EL0 itself (one word: mrs xN, tpidrro_el0); key page = pthread key * 8
#   Linux/Android   slot base = *(TPIDR_EL0 + 16) (mrs xN, tpidr_el0 ; ldr xN, [xN, #16]); key page = i * 8
# Neither fits HarmonyOS. Its musl lays the TLS blocks of all loaded modules out from TP + 0 with no gap (measured on the
# device: libbegetutil at TP+0, libsystemparam TP+1, libselinux TP+8.., libarkruntime TP+240, libmng_probe TP+424), so
# no word above TP is free, and TPIDRRO_EL0 is not writable. But a thread created after a library was loaded gets that
# library's TLS block at the same TP offset as every other such thread. So HarmonyOS uses the Darwin form with
# TPIDR_EL0: slot base = TP (one word: mrs xN, tpidr_el0), and the loader fills the key page with the TP offsets of
# libtotk_ohos.so's __thread slot array (runtime/host_tp.c, TOTK_OHOS), checked on every guest thread.
# The images then differ from the verified Darwin rewrite only in those mrs words (rewrite_ohos.py checks this).
#
# usage: python3 d2r_ohos.py <d2r folder> <tool.py> <tool arguments...>
import os
import runpy
import sys

d2r, tool = sys.argv[1], sys.argv[2]
os.environ['DM_HOST_TP'] = 'tpidr_el0'
sys.path.insert(0, d2r)
import hostreg  # noqa: E402  (after sys.path)

hostreg.LINUX = False                                   # one-word form: the slot base is the register itself
hostreg.HOST_TP_LEN = len(hostreg.host_tp_words(0))
assert hostreg.HOST_TP == 'tpidr_el0' and hostreg.HOST_TP_MRS == 0xD53BD040 and hostreg.HOST_TP_LEN == 1
sys.argv = [os.path.join(d2r, tool)] + sys.argv[3:]
runpy.run_path(sys.argv[0], run_name='__main__')
